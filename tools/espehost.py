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
