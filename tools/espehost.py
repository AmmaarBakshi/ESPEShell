#!/usr/bin/env python3
"""espehost - the laptop half of the ESPEShell host bridge.

Run this on the machine you want the ESP32 shell to be able to inspect:

    python tools/espehost.py 192.168.1.42          # or esp32.local

It dials out to the ESP32 (which is listening on HOST_BRIDGE_PORT, 2323 by
default), announces what it can do, and then answers questions: battery and
current draw, cameras, USB/HID/audio devices, CPU, memory, disks, network,
processes. From the shell those arrive as `hpower`, `hcam`, `hio`, and so on.

Outbound-only by design: your laptop never opens a listening port, and the
ESP32's address is the stable one.

Dependencies: none required. Richer output if these are present --
    pip install psutil            # per-core CPU, real process list, sensors
    pip install opencv-python     # camera capture and ASCII preview

Protocol (one JSON object per line, both directions):
    ESP32 -> here   {"id":7,"op":"power","args":"--watts"}
    here  -> ESP32  {"id":7,"ok":true,"text":"...","val":12.5}
                    {"id":7,"ok":false,"err":"no battery"}
    here  -> ESP32  {"ev":"hello","agent":"...","caps":"cpu mem ..."}
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import socket
import subprocess
import sys
import threading
import time
from typing import Callable, Dict, List, Optional, Tuple

__version__ = "1.0.0"

IS_WINDOWS = os.name == "nt"
IS_MAC = sys.platform == "darwin"
IS_LINUX = sys.platform.startswith("linux")

# Optional accelerators. Every op that uses one degrades to a stdlib path.
try:
    import psutil  # type: ignore
except ImportError:
    psutil = None


# ============================================================================
#  Reply
# ============================================================================

class OpError(Exception):
    """Raised by an op to return {"ok":false,"err":...} to the shell."""


class Reply:
    """What an op hands back.

    `text` is already formatted for an 80-column terminal - the laptop does the
    layout because the ESP32 has neither the width nor the heap to do it.
    `val` is the single number a fusion rule would want to threshold on, or
    None when the op has no natural scalar.
    """

    __slots__ = ("text", "val")

    def __init__(self, text: str, val: Optional[float] = None):
        self.text = text
        self.val = val


def table(rows: List[Tuple[str, object]], width: int = 9) -> str:
    """Render label/value pairs the way the ESP32's own commands do."""
    return "\n".join("%-*s %s" % (width, k, v) for k, v in rows)


def human_bytes(n: float) -> str:
    for unit in ("B", "K", "M", "G", "T"):
        if abs(n) < 1024.0 or unit == "T":
            return f"{n:.0f}{unit}" if unit == "B" else f"{n:.1f}{unit}"
        n /= 1024.0
    return f"{n:.1f}T"


def human_secs(s: float) -> str:
    s = int(s)
    d, s = divmod(s, 86400)
    h, s = divmod(s, 3600)
    m, s = divmod(s, 60)
    if d:
        return f"{d}d {h}h {m}m"
    if h:
        return f"{h}h {m}m"
    return f"{m}m {s}s"


# ============================================================================
#  Platform shims
# ============================================================================

def run_cmd(argv: List[str], timeout: float = 10.0) -> str:
    """Run a helper program and return stdout, or "" if it fails in any way."""
    try:
        p = subprocess.run(
            argv,
            capture_output=True,
            text=True,
            timeout=timeout,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0) if IS_WINDOWS else 0,
        )
        return p.stdout if p.returncode == 0 else ""
    except (OSError, subprocess.SubprocessError):
        return ""


def powershell(script: str, timeout: float = 15.0) -> str:
    """Run a PowerShell snippet on Windows. Empty string everywhere else."""
    if not IS_WINDOWS:
        return ""
    return run_cmd(
        ["powershell", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
         "-Command", script],
        timeout=timeout,
    )


def ps_json(script: str, timeout: float = 15.0):
    """PowerShell returning JSON. Always yields a list (possibly empty).

    ConvertTo-Json collapses a single object to a bare dict, so normalise.
    """
    out = powershell(script + " | ConvertTo-Json -Compress -Depth 4", timeout=timeout).strip()
    if not out:
        return []
    try:
        data = json.loads(out)
    except json.JSONDecodeError:
        return []
    if isinstance(data, dict):
        return [data]
    return data if isinstance(data, list) else []


def os_name() -> str:
    """Windows 11 still reports itself as "Windows 10" - the build number is
    the only thing that distinguishes them (11 starts at 22000)."""
    name = f"{platform.system()} {platform.release()}"
    if IS_WINDOWS and platform.release() == "10":
        try:
            build = int(platform.version().split(".")[2])
            if build >= 22000:
                name = "Windows 11"
        except (IndexError, ValueError):
            pass
    return name


def boot_time() -> float:
    if psutil:
        return psutil.boot_time()
    if IS_WINDOWS:
        try:
            import ctypes
            return time.time() - ctypes.windll.kernel32.GetTickCount64() / 1000.0
        except Exception:
            return 0.0
    try:
        with open("/proc/uptime") as f:
            return time.time() - float(f.read().split()[0])
    except OSError:
        return 0.0


# ============================================================================
#  Op registry
# ============================================================================

OPS: Dict[str, Callable[[List[str]], Reply]] = {}
OP_HELP: Dict[str, str] = {}


def op(name: str, help_text: str = "", enabled: bool = True):
    """Register an op under `name`. Disabled ops are not advertised in caps."""
    def deco(fn: Callable[[List[str]], Reply]):
        if enabled:
            OPS[name] = fn
            OP_HELP[name] = help_text or (fn.__doc__ or "").strip().split("\n")[0]
        return fn
    return deco


@op("caps", "list the ops this agent supports")
def op_caps(args: List[str]) -> Reply:
    width = max(len(k) for k in OP_HELP)
    body = "\n".join("  %-*s  %s" % (width, k, OP_HELP[k]) for k in sorted(OP_HELP))
    return Reply(f"{AGENT_NAME}\n{body}")


@op("ping", "round-trip check")
def op_ping(args: List[str]) -> Reply:
    return Reply("pong", 1)


# ---- system ----------------------------------------------------------------

@op("sys", "OS, machine, kernel, uptime")
def op_sys(args: List[str]) -> Reply:
    up = time.time() - boot_time() if boot_time() else 0
    rows = [
        ("host", socket.gethostname()),
        ("os", os_name()),
        ("build", platform.version()),
        ("arch", platform.machine()),
        ("python", platform.python_version()),
        ("user", os.environ.get("USERNAME") or os.environ.get("USER") or "?"),
    ]
    if up:
        rows.append(("uptime", human_secs(up)))
    rows.append(("agent", f"espehost {__version__}"))
    return Reply(table(rows), up)


@op("cpu", "model, cores, per-core load")
def op_cpu(args: List[str]) -> Reply:
    model = platform.processor() or platform.machine()
    cores_logical = os.cpu_count() or 0
    rows: List[Tuple[str, object]] = []

    if IS_WINDOWS and (not model or model.startswith(("Intel64", "AMD64", "ARM"))):
        for item in ps_json("Get-CimInstance Win32_Processor | "
                            "Select-Object Name,MaxClockSpeed,NumberOfCores"):
            model = (item.get("Name") or model).strip()
            if item.get("NumberOfCores"):
                rows.append(("cores", f"{item['NumberOfCores']} physical, {cores_logical} logical"))
            if item.get("MaxClockSpeed"):
                rows.append(("max", f"{item['MaxClockSpeed']} MHz"))
            break

    rows.insert(0, ("model", model or "unknown"))
    if not any(k == "cores" for k, _ in rows):
        rows.append(("cores", cores_logical))

    total: Optional[float] = None
    if psutil:
        per = psutil.cpu_percent(interval=0.3, percpu=True)
        total = psutil.cpu_percent(interval=None)
        rows.append(("load", f"{total:.0f}%"))
        rows.append(("per-core", " ".join(f"{p:.0f}%" for p in per)))
        freq = psutil.cpu_freq()
        if freq and freq.current:
            rows.append(("now", f"{freq.current:.0f} MHz"))
    elif IS_WINDOWS:
        out = ps_json("Get-CimInstance Win32_Processor | Select-Object LoadPercentage")
        if out and out[0].get("LoadPercentage") is not None:
            total = float(out[0]["LoadPercentage"])
            rows.append(("load", f"{total:.0f}%"))
    else:
        try:
            load1 = os.getloadavg()[0]
            total = 100.0 * load1 / max(cores_logical, 1)
            rows.append(("load", f"{load1:.2f} (1 min avg)"))
        except (OSError, AttributeError):
            pass

    if total is None:
        rows.append(("load", "unavailable (pip install psutil)"))
    return Reply(table(rows), total)


@op("mem", "RAM and swap usage")
def op_mem(args: List[str]) -> Reply:
    if psutil:
        vm = psutil.virtual_memory()
        sw = psutil.swap_memory()
        rows = [
            ("total", human_bytes(vm.total)),
            ("used", f"{human_bytes(vm.used)}  ({vm.percent:.0f}%)"),
            ("free", human_bytes(vm.available)),
            ("swap", f"{human_bytes(sw.used)} / {human_bytes(sw.total)}"),
        ]
        return Reply(table(rows), vm.percent)

    if IS_WINDOWS:
        for item in ps_json("Get-CimInstance Win32_OperatingSystem | "
                            "Select-Object TotalVisibleMemorySize,FreePhysicalMemory"):
            total = float(item.get("TotalVisibleMemorySize") or 0) * 1024
            free = float(item.get("FreePhysicalMemory") or 0) * 1024
            if total:
                pct = 100.0 * (total - free) / total
                return Reply(table([
                    ("total", human_bytes(total)),
                    ("used", f"{human_bytes(total - free)}  ({pct:.0f}%)"),
                    ("free", human_bytes(free)),
                ]), pct)
    else:
        try:
            info = {}
            with open("/proc/meminfo") as f:
                for line in f:
                    k, _, v = line.partition(":")
                    info[k] = float(v.split()[0]) * 1024
            total, avail = info.get("MemTotal", 0), info.get("MemAvailable", 0)
            if total:
                pct = 100.0 * (total - avail) / total
                return Reply(table([
                    ("total", human_bytes(total)),
                    ("used", f"{human_bytes(total - avail)}  ({pct:.0f}%)"),
                    ("free", human_bytes(avail)),
                ]), pct)
        except (OSError, ValueError, IndexError):
            pass

    raise OpError("memory information unavailable (pip install psutil)")


@op("disk", "mounted filesystems and usage")
def op_disk(args: List[str]) -> Reply:
    lines = ["%-12s %8s %8s %8s  %s" % ("device", "size", "used", "free", "mount")]
    worst = 0.0

    mounts: List[Tuple[str, str]] = []
    if psutil:
        mounts = [(p.device, p.mountpoint) for p in psutil.disk_partitions(all=False)]
    elif IS_WINDOWS:
        mounts = [(f"{d}:", f"{d}:\\") for d in "CDEFGHIJKLMNOPQRSTUVWXYZ"
                  if os.path.exists(f"{d}:\\")]
    else:
        mounts = [("/", "/")]

    for device, mount in mounts:
        try:
            usage = shutil.disk_usage(mount)
        except OSError:
            continue    # empty optical drive, unreadable mount, disconnected share
        pct = 100.0 * usage.used / usage.total if usage.total else 0
        worst = max(worst, pct)
        lines.append("%-12s %8s %8s %8s  %s (%.0f%%)" % (
            device[:12], human_bytes(usage.total), human_bytes(usage.used),
            human_bytes(usage.free), mount, pct))

    if len(lines) == 1:
        raise OpError("no readable filesystems")
    return Reply("\n".join(lines), worst)


# ---- power -----------------------------------------------------------------

def _power_windows() -> Tuple[List[Tuple[str, object]], Optional[float], Optional[float]]:
    """(rows, percent, watts). Watts is signed: + charging, - discharging.

    The useful numbers live in the root\\WMI namespace, not Win32_Battery:
    BatteryStatus carries the instantaneous charge/discharge rate in mW and
    the pack voltage in mV, which together give the actual current draw.
    Win32_Battery only has the rounded percentage.
    """
    rows: List[Tuple[str, object]] = []
    status = ps_json(r"Get-CimInstance -Namespace root\WMI -ClassName BatteryStatus "
                     r"-ErrorAction SilentlyContinue | Select-Object "
                     r"Voltage,ChargeRate,DischargeRate,Charging,Discharging,"
                     r"PowerOnline,RemainingCapacity")
    summary = ps_json(r"Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue | "
                      r"Select-Object EstimatedChargeRemaining,EstimatedRunTime")
    full = ps_json(r"Get-CimInstance -Namespace root\WMI -ClassName "
                   r"BatteryFullChargedCapacity -ErrorAction SilentlyContinue | "
                   r"Select-Object FullChargedCapacity")
    design = ps_json(r"Get-CimInstance -Namespace root\WMI -ClassName BatteryStaticData "
                     r"-ErrorAction SilentlyContinue | Select-Object DesignedCapacity")

    if not status and not summary:
        raise OpError("no battery (desktop, or the ACPI battery driver is absent)")

    st = status[0] if status else {}
    charging = bool(st.get("Charging"))
    on_ac = bool(st.get("PowerOnline"))
    mv = float(st.get("Voltage") or 0)
    charge_mw = float(st.get("ChargeRate") or 0)
    drain_mw = float(st.get("DischargeRate") or 0)

    pct: Optional[float] = None
    if summary and summary[0].get("EstimatedChargeRemaining") is not None:
        pct = float(summary[0]["EstimatedChargeRemaining"])
    elif full and st.get("RemainingCapacity"):
        cap = float(full[0].get("FullChargedCapacity") or 0)
        if cap:
            pct = 100.0 * float(st["RemainingCapacity"]) / cap

    rows.append(("source", "AC adapter" if on_ac else "battery"))
    rows.append(("state", "charging" if charging else
                          "discharging" if drain_mw else
                          "full" if on_ac else "idle"))
    if pct is not None:
        rows.append(("charge", f"{pct:.0f}%"))

    # Signed so a fusion rule can say "draw more negative than -20W".
    watts: Optional[float] = None
    mw = charge_mw if charging else -drain_mw
    if mw:
        watts = mw / 1000.0
        rows.append(("power", f"{watts:+.2f} W  ({abs(mw):.0f} mW {'in' if mw > 0 else 'out'})"))
        if mv:
            rows.append(("current", f"{abs(mw) / (mv / 1000.0):.0f} mA at {mv / 1000.0:.2f} V"))
    elif mv:
        rows.append(("voltage", f"{mv / 1000.0:.2f} V"))

    if full and full[0].get("FullChargedCapacity"):
        cap = float(full[0]["FullChargedCapacity"])
        remain = float(st.get("RemainingCapacity") or 0)
        rows.append(("capacity", f"{remain:.0f} / {cap:.0f} mWh"))
        if design and design[0].get("DesignedCapacity"):
            dcap = float(design[0]["DesignedCapacity"])
            if dcap:
                rows.append(("health", f"{100.0 * cap / dcap:.0f}% of design ({dcap:.0f} mWh)"))

    # EstimatedRunTime is 0xFFFFFFFF/60 minutes when Windows has no estimate.
    if summary and summary[0].get("EstimatedRunTime") is not None:
        mins = int(summary[0]["EstimatedRunTime"])
        if 0 < mins < 60 * 24 * 7:
            rows.append(("remaining", human_secs(mins * 60)))
    return rows, pct, watts


def _power_posix() -> Tuple[List[Tuple[str, object]], Optional[float], Optional[float]]:
    rows: List[Tuple[str, object]] = []
    pct: Optional[float] = None
    watts: Optional[float] = None

    if psutil and hasattr(psutil, "sensors_battery"):
        bat = psutil.sensors_battery()
        if bat is not None:
            pct = float(bat.percent)
            rows.append(("source", "AC adapter" if bat.power_plugged else "battery"))
            rows.append(("charge", f"{pct:.0f}%"))
            if bat.secsleft not in (getattr(psutil, "POWER_TIME_UNLIMITED", -1),
                                    getattr(psutil, "POWER_TIME_UNKNOWN", -2)):
                rows.append(("remaining", human_secs(bat.secsleft)))

    # Linux exposes the instantaneous draw; psutil does not surface it.
    base = "/sys/class/power_supply"
    if IS_LINUX and os.path.isdir(base):
        for entry in sorted(os.listdir(base)):
            path = os.path.join(base, entry)

            def read(field: str) -> Optional[float]:
                try:
                    with open(os.path.join(path, field)) as f:
                        return float(f.read().strip())
                except (OSError, ValueError):
                    return None

            power_uw = read("power_now")
            if power_uw is None:
                cur_ua, volt_uv = read("current_now"), read("voltage_now")
                if cur_ua is not None and volt_uv is not None:
                    power_uw = cur_ua * volt_uv / 1e6
            if power_uw:
                watts = power_uw / 1e6
                rows.append(("power", f"{watts:.2f} W"))
                volt_uv = read("voltage_now")
                if volt_uv:
                    rows.append(("current", f"{power_uw / volt_uv * 1e3:.0f} mA "
                                            f"at {volt_uv / 1e6:.2f} V"))
                break

    if not rows:
        raise OpError("no battery information available (pip install psutil)")
    return rows, pct, watts


@op("power", "battery, charge state, current draw")
def op_power(args: List[str]) -> Reply:
    rows, pct, watts = _power_windows() if IS_WINDOWS else _power_posix()
    # Default scalar is the charge percentage - "battery under 20" is the rule
    # people actually write. --watts switches it to the draw.
    val = watts if "--watts" in args or "-w" in args else pct
    return Reply(table(rows), val)


# ---- cameras ---------------------------------------------------------------

# Dark to light. Rendered on a dark terminal, so index 0 is the dimmest pixel.
ASCII_RAMP = " .:-=+*#%@"

# How long to let a camera's auto-exposure settle before keeping the frame.
CAM_WARMUP_S = 1.2

# Sampling window for per-process CPU. Long enough to be meaningful, short
# enough that the shell does not appear to hang.
PROC_SAMPLE_S = 0.4


def _cv2():
    try:
        import cv2  # type: ignore
        return cv2
    except ImportError:
        raise OpError("camera capture needs OpenCV (pip install opencv-python)")


def _camera_names() -> List[str]:
    """Device names in the order the capture backend will index them.

    Best effort: enumeration and capture go through different subsystems on
    every OS, so treat the names as labels and the index as the real handle.
    """
    if IS_WINDOWS:
        items = ps_json(r"Get-PnpDevice -Class Camera,Image -Status OK "
                        r"-ErrorAction SilentlyContinue | Select-Object FriendlyName")
        return [i.get("FriendlyName", "?") for i in items if i.get("FriendlyName")]
    if IS_LINUX:
        names = []
        for n in range(10):
            dev = f"/dev/video{n}"
            if not os.path.exists(dev):
                continue
            label = dev
            try:
                with open(f"/sys/class/video4linux/video{n}/name") as f:
                    label = f"{f.read().strip()} ({dev})"
            except OSError:
                pass
            names.append(label)
        return names
    if IS_MAC:
        out = run_cmd(["system_profiler", "SPCameraDataType"])
        return [ln.strip().rstrip(":") for ln in out.splitlines()
                if ln.strip().endswith(":") and not ln.startswith(" " * 8)][1:]
    return []


def _arg_value(args: List[str], flag: str) -> Optional[str]:
    if flag in args:
        i = args.index(flag)
        if i + 1 < len(args):
            return args[i + 1]
    return None


def _first_int(args: List[str], default: int = 0) -> int:
    for a in args:
        if a.isdigit():
            return int(a)
    return default


def _grab_frame(index: int):
    """Open camera `index`, discard warm-up frames, return one good frame."""
    cv2 = _cv2()
    # CAP_DSHOW avoids the several-second MSMF open penalty on Windows.
    backend = getattr(cv2, "CAP_DSHOW", 0) if IS_WINDOWS else 0
    cap = cv2.VideoCapture(index, backend) if backend else cv2.VideoCapture(index)
    if not cap.isOpened():
        cap.release()
        raise OpError(f"cannot open camera {index} (in use by another app?)")
    try:
        # A webcam's first frames are black, then progressively exposed: its
        # auto-exposure needs a few hundred ms to settle. Keep reading until
        # the image stops getting brighter, or the budget runs out.
        frame = None
        deadline = time.monotonic() + CAM_WARMUP_S
        last_mean = -1.0
        while time.monotonic() < deadline:
            ok, candidate = cap.read()
            if not ok or candidate is None:
                continue
            frame = candidate
            mean = float(candidate.mean())
            if last_mean >= 0 and mean <= last_mean * 1.02:
                break               # settled: no meaningful gain from waiting
            last_mean = mean
        if frame is None:
            raise OpError(f"camera {index} opened but returned no frame")
        return frame
    finally:
        cap.release()


@op("cam", "list cameras, snapshot, ASCII preview")
def op_cam(args: List[str]) -> Reply:
    sub = args[0] if args and not args[0].startswith("-") and not args[0].isdigit() else "list"
    rest = args[1:] if sub != "list" or (args and args[0] == "list") else args

    if sub == "list":
        names = _camera_names()
        if not names:
            raise OpError("no cameras detected")
        body = "\n".join(f"  [{i}] {n}" for i, n in enumerate(names))
        return Reply(f"{len(names)} camera(s):\n{body}", len(names))

    if sub == "snap":
        cv2 = _cv2()
        index = _first_int(rest)
        path = _arg_value(rest, "-o") or os.path.join(
            os.path.expanduser("~"), f"espeshell-cam{index}-{int(time.time())}.jpg")
        frame = _grab_frame(index)
        if not cv2.imwrite(path, frame):
            raise OpError(f"could not write {path}")
        h, w = frame.shape[:2]
        size = os.path.getsize(path)
        return Reply(table([
            ("camera", f"[{index}]"),
            ("size", f"{w}x{h}"),
            ("saved", path),
            ("bytes", human_bytes(size)),
        ]), size)

    if sub in ("ascii", "view", "preview"):
        cv2 = _cv2()
        index = _first_int(rest)
        cols = int(_arg_value(rest, "-w") or 64)
        cols = max(16, min(cols, 120))          # keep the reply inside one line buffer
        frame = _grab_frame(index)
        h, w = frame.shape[:2]
        # Terminal cells are about twice as tall as they are wide.
        rows = max(8, int(cols * h / w / 2))
        small = cv2.resize(cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY), (cols, rows),
                           interpolation=cv2.INTER_AREA)
        mean = float(small.mean())
        lo, hi = float(small.min()), float(small.max())

        # Ten ramp characters over the full 0-255 range wastes most of them on
        # a dim scene. Stretch to the range actually present unless asked not
        # to, so an indoor frame is legible instead of a block of spaces.
        shown = small
        if "--raw" not in rest and hi - lo > 4:
            shown = cv2.normalize(small, None, 0, 255, cv2.NORM_MINMAX)

        scale = len(ASCII_RAMP) - 1
        art = "\n".join(
            "".join(ASCII_RAMP[int(px) * scale // 255] for px in row)
            for row in shown
        )

        header = f"camera [{index}]  {w}x{h} -> {cols}x{rows}  mean {mean:.0f}/255"
        if hi - lo <= 4:
            # Otherwise this prints as a blank rectangle and looks like a bug.
            header += "\n(frame is featureless - privacy shutter closed, or a dark room)"
        return Reply(f"{header}\n{art}", mean)

    raise OpError(f"cam: unknown subcommand '{sub}' (list | snap | ascii)")


# ---- input / output devices -------------------------------------------------

# PnP classes worth showing, in the order a person would look for them.
# Left is the `io` subcommand, right is the Windows device class.
IO_CLASSES = [
    ("usb", "USB"),
    ("hid", "HIDClass"),
    ("keyboard", "Keyboard"),
    ("mouse", "Mouse"),
    ("audio", "AudioEndpoint"),
    ("media", "MEDIA"),
    ("display", "Display"),
    ("monitor", "Monitor"),
    ("net", "Net"),
    ("camera", "Camera"),
    ("bluetooth", "Bluetooth"),
    ("printer", "Printer"),
    ("disk", "DiskDrive"),
]


def _pnp_devices(classes: List[str]) -> List[Tuple[str, str, str]]:
    """(class, name, status) for the given Windows device classes."""
    quoted = ",".join(f"'{c}'" for c in classes)
    items = ps_json(f"Get-PnpDevice -Class {quoted} -ErrorAction SilentlyContinue | "
                    f"Select-Object Class,FriendlyName,Status", timeout=25.0)
    out = []
    for i in items:
        name = i.get("FriendlyName")
        if name:
            out.append((i.get("Class", "?"), name, i.get("Status", "?")))
    return out


def _serial_ports() -> List[str]:
    if IS_WINDOWS:
        items = ps_json(r"Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue | "
                        r"Select-Object DeviceID,Caption")
        if items:
            return [f"{i.get('DeviceID','?')}  {i.get('Caption','')}".strip() for i in items]
        # Win32_SerialPort misses USB-UART bridges; the registry does not.
        out = powershell(r"Get-ItemProperty HKLM:\HARDWARE\DEVICEMAP\SERIALCOMM "
                         r"-ErrorAction SilentlyContinue | Out-String")
        return [ln.strip() for ln in out.splitlines()
                if ln.strip().startswith(("\\Device", "Device"))]
    ports = []
    for prefix in ("/dev/ttyUSB", "/dev/ttyACM", "/dev/ttyS", "/dev/tty.usb", "/dev/cu.usb"):
        for n in range(8):
            for cand in (f"{prefix}{n}", prefix):
                if os.path.exists(cand) and cand not in ports:
                    ports.append(cand)
    return ports


@op("io", "USB/HID/audio/display devices and serial ports")
def op_io(args: List[str]) -> Reply:
    want = [a.lower() for a in args if not a.startswith("-")]

    if want and want[0] == "serial":
        ports = _serial_ports()
        if not ports:
            raise OpError("no serial ports found")
        return Reply("serial ports:\n" + "\n".join(f"  {p}" for p in ports), len(ports))

    if not IS_WINDOWS:
        if IS_LINUX and shutil.which("lsusb"):
            out = run_cmd(["lsusb"]).strip()
            if out:
                return Reply(out, len(out.splitlines()))
        if IS_MAC:
            out = run_cmd(["system_profiler", "SPUSBDataType"], timeout=20).strip()
            if out:
                return Reply(out[:6000], None)
        raise OpError("device enumeration needs lsusb (Linux) or PowerShell (Windows)")

    selected = [cls for key, cls in IO_CLASSES if not want or key in want]
    if not selected:
        keys = " ".join(k for k, _ in IO_CLASSES)
        raise OpError(f"io: unknown category. try one of: {keys} serial")

    devices = _pnp_devices(selected)
    if not devices:
        raise OpError("no matching devices")

    # Group by class so a bare `hio` is scannable rather than a flat wall.
    by_class: Dict[str, List[Tuple[str, str]]] = {}
    for cls, name, status in devices:
        by_class.setdefault(cls, []).append((name, status))

    lines = []
    for cls in sorted(by_class):
        entries = sorted(by_class[cls])
        lines.append(f"{cls}  ({len(entries)})")
        for name, status in entries:
            flag = "" if status == "OK" else f"   [{status}]"
            lines.append(f"  {name[:66]}{flag}")
    return Reply("\n".join(lines), len(devices))


# ---- network ----------------------------------------------------------------

@op("net", "interfaces, addresses, link state")
def op_net(args: List[str]) -> Reply:
    if "conn" in args or "-c" in args:
        if not psutil:
            raise OpError("connection list needs psutil (pip install psutil)")
        lines = ["%-24s %-24s %-12s %s" % ("local", "remote", "state", "pid")]
        for c in psutil.net_connections(kind="inet")[:40]:
            laddr = f"{c.laddr.ip}:{c.laddr.port}" if c.laddr else "-"
            raddr = f"{c.raddr.ip}:{c.raddr.port}" if c.raddr else "-"
            lines.append("%-24s %-24s %-12s %s" % (laddr[:24], raddr[:24], c.status, c.pid or "-"))
        return Reply("\n".join(lines), len(lines) - 1)

    if not psutil:
        raise OpError("interface details need psutil (pip install psutil)")

    addrs = psutil.net_if_addrs()
    stats = psutil.net_if_stats()
    counters = psutil.net_io_counters(pernic=True)
    lines = []
    for name in sorted(addrs):
        st = stats.get(name)
        if st and not st.isup and "-a" not in args:
            continue        # skip the pile of down virtual adapters by default
        speed = f"{st.speed} Mb/s" if st and st.speed else "-"
        lines.append(f"{name}  [{'up' if st and st.isup else 'down'}, {speed}]")
        for a in addrs[name]:
            if a.family == socket.AF_INET:
                lines.append(f"    inet  {a.address}/{a.netmask or '?'}")
            elif a.family == socket.AF_INET6:
                lines.append(f"    inet6 {a.address.split('%')[0]}")
            elif getattr(a.family, "name", "") in ("AF_LINK", "AF_PACKET"):
                lines.append(f"    ether {a.address}")
        io = counters.get(name)
        if io:
            lines.append(f"    rx {human_bytes(io.bytes_recv)}  tx {human_bytes(io.bytes_sent)}")
    if not lines:
        raise OpError("no interfaces up")
    return Reply("\n".join(lines), len([l for l in lines if not l.startswith(" ")]))


# ---- processes ---------------------------------------------------------------

@op("proc", "top processes by CPU or memory")
def op_proc(args: List[str]) -> Reply:
    count = 15
    for a in args:
        if a.isdigit():
            count = max(1, min(int(a), 50))
    by_mem = "-m" in args or "mem" in args

    if not psutil:
        if IS_WINDOWS:
            key = "WS" if by_mem else "CPU"
            out = powershell(f"Get-Process | Sort-Object {key} -Descending | "
                             f"Select-Object -First {count} Id,ProcessName,WS,CPU | "
                             f"Format-Table -AutoSize | Out-String -Width 78")
            if out.strip():
                return Reply(out.strip(), count)
        raise OpError("process list needs psutil (pip install psutil)")

    # cpu_percent() is a delta since the last call *on that same Process
    # object*, and the first call always returns 0.0. So prime every process,
    # wait, then read the same objects back - process_iter() alone would report
    # a screen full of zeroes.
    handles = list(psutil.process_iter(["pid", "name"]))
    for p in handles:
        try:
            p.cpu_percent(None)
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
    time.sleep(PROC_SAMPLE_S)

    cores = os.cpu_count() or 1
    procs = []
    for p in handles:
        try:
            # Normalise to whole-machine percent: psutil reports per-core, so a
            # busy thread reads 100% on an 8-core box where top would say 12%.
            cpu = p.cpu_percent(None) / cores
            rss = p.memory_info().rss
            procs.append((p.info["pid"], p.info["name"] or "?", cpu, rss))
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue      # process exited, or is protected: both are expected

    procs.sort(key=lambda r: r[3] if by_mem else r[2], reverse=True)
    lines = ["%7s  %-28s %7s %9s" % ("pid", "name", "cpu", "rss")]
    for pid, name, cpu, rss in procs[:count]:
        lines.append("%7d  %-28s %6.1f%% %9s" % (pid, name[:28], cpu, human_bytes(rss)))
    return Reply("\n".join(lines), len(procs))


# ---- sensors -----------------------------------------------------------------

@op("temp", "thermal sensors and fans")
def op_temp(args: List[str]) -> Reply:
    rows: List[Tuple[str, object]] = []
    hottest: Optional[float] = None

    if psutil and hasattr(psutil, "sensors_temperatures"):
        for chip, entries in (psutil.sensors_temperatures() or {}).items():
            for e in entries:
                label = f"{chip}/{e.label}" if e.label else chip
                rows.append((label[:18], f"{e.current:.1f} C"))
                hottest = e.current if hottest is None else max(hottest, e.current)
    if psutil and hasattr(psutil, "sensors_fans"):
        for chip, entries in (psutil.sensors_fans() or {}).items():
            for e in entries:
                rows.append((f"fan {e.label or chip}"[:18], f"{e.current} rpm"))

    if not rows and IS_WINDOWS:
        # Most consumer laptops do not expose MSAcpi_ThermalZoneTemperature,
        # and the ones that do need an elevated shell. Try anyway, then say so.
        for item in ps_json(r"Get-CimInstance -Namespace root\WMI -ClassName "
                            r"MSAcpi_ThermalZoneTemperature -ErrorAction SilentlyContinue | "
                            r"Select-Object InstanceName,CurrentTemperature"):
            raw = item.get("CurrentTemperature")
            if raw:
                celsius = float(raw) / 10.0 - 273.15    # tenths of a kelvin
                name = str(item.get("InstanceName", "zone")).split("\\")[-1]
                rows.append((name[:18], f"{celsius:.1f} C"))
                hottest = celsius if hottest is None else max(hottest, celsius)

    if not rows:
        raise OpError("no thermal sensors readable "
                      "(Windows rarely exposes these without vendor drivers)")
    return Reply(table(rows, width=18), hottest)


@op("gpu", "graphics adapters")
def op_gpu(args: List[str]) -> Reply:
    if IS_WINDOWS:
        items = ps_json(r"Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | "
                        r"Select-Object Name,AdapterRAM,DriverVersion,"
                        r"CurrentHorizontalResolution,CurrentVerticalResolution")
        if not items:
            raise OpError("no video controllers reported")
        lines = []
        for i in items:
            lines.append(i.get("Name", "?"))
            ram = i.get("AdapterRAM")
            # AdapterRAM is a signed 32-bit field: >=4GB wraps to a negative.
            if ram and int(ram) > 0:
                lines.append(f"    vram   {human_bytes(int(ram))}")
            hres = i.get("CurrentHorizontalResolution")
            vres = i.get("CurrentVerticalResolution")
            if hres and vres:
                lines.append(f"    mode   {hres}x{vres}")
            if i.get("DriverVersion"):
                lines.append(f"    driver {i['DriverVersion']}")
        return Reply("\n".join(lines), len(items))

    if shutil.which("nvidia-smi"):
        out = run_cmd(["nvidia-smi", "--query-gpu=name,memory.used,memory.total,utilization.gpu",
                       "--format=csv,noheader"]).strip()
        if out:
            return Reply(out, None)
    if IS_LINUX and shutil.which("lspci"):
        out = "\n".join(l for l in run_cmd(["lspci"]).splitlines()
                        if "VGA" in l or "3D controller" in l)
        if out:
            return Reply(out, None)
    raise OpError("no GPU information available")


# ============================================================================
#  Agent
# ============================================================================

AGENT_NAME = f"espehost/{__version__} on {socket.gethostname()} ({platform.system()})"


class Agent:
    def __init__(self, host: str, port: int, retry: float = 3.0, verbose: bool = False):
        self.host = host
        self.port = port
        self.retry = retry
        self.verbose = verbose
        self.sock: Optional[socket.socket] = None
        self._wlock = threading.Lock()

    # -- logging -------------------------------------------------------------
    def log(self, *parts: object) -> None:
        print(time.strftime("[%H:%M:%S]"), *parts, flush=True)

    def vlog(self, *parts: object) -> None:
        if self.verbose:
            self.log(*parts)

    # -- wire ----------------------------------------------------------------
    def send(self, obj: dict) -> None:
        if not self.sock:
            return
        line = json.dumps(obj, separators=(",", ":"), ensure_ascii=True) + "\n"
        with self._wlock:
            try:
                self.sock.sendall(line.encode("utf-8"))
            except OSError as exc:
                self.vlog("send failed:", exc)

    def event(self, text: str, kind: str = "note") -> None:
        """Push an unsolicited message; it prints in the shell as [host] ..."""
        self.send({"ev": kind, "text": text})

    def handle(self, req: dict) -> None:
        req_id = req.get("id")
        name = req.get("op", "")
        args = str(req.get("args", "")).split()

        fn = OPS.get(name)
        if fn is None:
            self.send({"id": req_id, "ok": False,
                       "err": f"unknown op '{name}' (try: host caps)"})
            return
        try:
            reply = fn(args)
        except OpError as exc:
            self.send({"id": req_id, "ok": False, "err": str(exc)})
            return
        except Exception as exc:                    # a bug here must not kill the link
            self.log(f"op '{name}' raised:", repr(exc))
            self.send({"id": req_id, "ok": False,
                       "err": f"{name}: {type(exc).__name__}: {exc}"})
            return

        out = {"id": req_id, "ok": True, "text": reply.text}
        if reply.val is not None:
            out["val"] = round(float(reply.val), 4)
        self.send(out)

    # -- session -------------------------------------------------------------
    def serve_once(self) -> None:
        sock = socket.create_connection((self.host, self.port), timeout=10)
        sock.settimeout(None)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock = sock
        self.log(f"connected to {self.host}:{self.port}")
        self.send({"ev": "hello", "agent": AGENT_NAME, "caps": " ".join(sorted(OPS))})

        buf = b""
        try:
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    self.log("ESP32 closed the link")
                    return
                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    raw = raw.strip()
                    if not raw:
                        continue
                    try:
                        req = json.loads(raw.decode("utf-8", "replace"))
                    except json.JSONDecodeError:
                        self.vlog("ignoring malformed line:", raw[:120])
                        continue
                    self.vlog("<-", req)
                    self.handle(req)
        finally:
            self.sock = None
            try:
                sock.close()
            except OSError:
                pass

    def run(self) -> None:
        self.log(AGENT_NAME)
        self.log(f"ops: {' '.join(sorted(OPS))}")
        while True:
            try:
                self.serve_once()
            except KeyboardInterrupt:
                raise
            except OSError as exc:
                self.vlog(f"connect failed: {exc}")
            if self.retry <= 0:
                return
            time.sleep(self.retry)


# ============================================================================
#  Entry point
# ============================================================================

def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        prog="espehost",
        description="Expose this laptop's hardware to an ESPEShell session.",
    )
    ap.add_argument("host", help="ESP32 address (IP, or esp32.local)")
    ap.add_argument("-p", "--port", type=int, default=2323,
                    help="bridge port on the ESP32 (default: 2323)")
    ap.add_argument("-r", "--retry", type=float, default=3.0,
                    help="seconds between reconnect attempts, 0 to exit instead")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="log every request and reply")
    ap.add_argument("-V", "--version", action="version", version=f"espehost {__version__}")
    opts = ap.parse_args(argv)

    agent = Agent(opts.host, opts.port, retry=opts.retry, verbose=opts.verbose)
    try:
        agent.run()
    except KeyboardInterrupt:
        print()
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
