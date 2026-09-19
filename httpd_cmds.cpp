#include "shell.h"
#include "config.h"
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>

// ============================================================================
//  httpd - a small web front end for the board's filesystem.
//
//  Browse directories, download files and upload new ones from any browser on
//  the network; handy when a file is bigger than a comfortable base64 paste
//  through `recv`. The server is polled from loop(), so it never blocks the
//  shell, and it only runs while you ask it to.
// ============================================================================

static WebServer *s_srv = nullptr;
static uint16_t   s_port = 80;
static uint32_t   s_hits = 0;
static File       s_upFile;
static String     s_upName;

static String contentTypeFor(const String &path) {
  String p = path;
  p.toLowerCase();
  if (p.endsWith(".htm") || p.endsWith(".html")) return "text/html";
  if (p.endsWith(".css"))  return "text/css";
  if (p.endsWith(".js"))   return "application/javascript";
  if (p.endsWith(".json")) return "application/json";
  if (p.endsWith(".png"))  return "image/png";
  if (p.endsWith(".jpg") || p.endsWith(".jpeg")) return "image/jpeg";
  if (p.endsWith(".gif"))  return "image/gif";
  if (p.endsWith(".svg"))  return "image/svg+xml";
  if (p.endsWith(".ico"))  return "image/x-icon";
  if (p.endsWith(".txt") || p.endsWith(".sh") || p.endsWith(".log")) return "text/plain";
  return "application/octet-stream";
}

static String htmlEscape(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

static void sendDirListing(const String &dir) {
  String html = F("<!doctype html><meta charset=utf-8>"
                  "<meta name=viewport content='width=device-width'>"
                  "<style>body{font:14px system-ui,sans-serif;margin:2rem;max-width:44rem}"
                  "a{text-decoration:none}td{padding:.2rem .8rem .2rem 0}"
                  "h1{font-size:1.1rem}form{margin-top:1.5rem}</style>");
  html += "<h1>" + htmlEscape(g_hostname) + ":" + htmlEscape(dir) + "</h1><table>";

  if (dir != "/") {
    String up = dirName(dir);
    html += "<tr><td><a href=\"" + htmlEscape(up) + "\">../</a></td><td></td></tr>";
  }

  File d = LittleFS.open(dir);
  if (d && d.isDirectory()) {
    File e = d.openNextFile();
    while (e) {
      String name = String(e.name());
      int sl = name.lastIndexOf('/');
      if (sl >= 0) name = name.substring(sl + 1);
      String href = dir;
      if (!href.endsWith("/")) href += "/";
      href += name;
      bool isdir = e.isDirectory();
      size_t sz = isdir ? 0 : e.size();
      html += "<tr><td><a href=\"" + htmlEscape(href) + "\">" + htmlEscape(name) +
              (isdir ? "/" : "") + "</a></td><td>" +
              (isdir ? String("-") : humanBytes(sz)) + "</td></tr>";
      e.close();
      e = d.openNextFile();
    }
    d.close();
  }

  html += F("</table>"
            "<form method=\"POST\" action=\"/__upload\" enctype=\"multipart/form-data\">"
            "<input type=\"file\" name=\"f\"> <input type=\"submit\" value=\"Upload here\">"
            "<input type=\"hidden\" name=\"dir\" value=\"");
  html += htmlEscape(dir);
  html += F("\"></form><p style=\"color:#888\">ESPEShell httpd</p>");
  s_srv->send(200, "text/html", html);
}

static void handleRequest() {
  s_hits++;
  String uri = s_srv->uri();
  String abs = normalizePath(uri.length() ? uri : String("/"));

  if (isDir(abs)) { sendDirListing(abs); return; }

  File f = LittleFS.open(abs, "r");
  if (!f || f.isDirectory()) {
    if (f) f.close();
    s_srv->send(404, "text/plain", "404: " + abs + " not found\n");
    return;
  }
  s_srv->streamFile(f, contentTypeFor(abs));
  f.close();
}

// Uploads arrive in chunks: open on START, append on WRITE, close on END.
static void handleUploadData() {
  HTTPUpload &up = s_srv->upload();
  if (up.status == UPLOAD_FILE_START) {
    String dir = s_srv->hasArg("dir") ? s_srv->arg("dir") : String("/");
    String name = up.filename;
    int sl = name.lastIndexOf('/');
    if (sl >= 0) name = name.substring(sl + 1);   // ignore any path the client sends
    if (!dir.endsWith("/")) dir += "/";
    s_upName = normalizePath(dir + name);
    s_upFile = LittleFS.open(s_upName, "w");
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (s_upFile) s_upFile.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (s_upFile) s_upFile.close();
  }
}

static void handleUploadDone() {
  String dir = s_srv->hasArg("dir") ? s_srv->arg("dir") : String("/");
  s_srv->sendHeader("Location", dir.length() ? dir : String("/"));
  s_srv->send(303, "text/plain", "uploaded " + s_upName + "\n");
}

// Called once per loop() iteration from ESPEShell.ino.
void httpdPoll() {
  if (s_srv) s_srv->handleClient();
}

bool httpdRunning() { return s_srv != nullptr; }

static int cmd_httpd(int argc, char **argv, ShellIO &io) {
  String sub = (argc >= 2) ? String(argv[1]) : String("status");

  if (sub == "start") {
    if (s_srv) { io.out.printf("httpd: already running on port %u\n", s_port); return 1; }
    if (WiFi.status() != WL_CONNECTED) { io.out.println(F("httpd: WiFi not connected")); return 1; }
    uint16_t port = (argc >= 3) ? (uint16_t)String(argv[2]).toInt() : 80;
    if (port == 0) { io.out.println(F("httpd: bad port")); return 1; }

    s_srv = new WebServer(port);
    s_port = port;
    s_hits = 0;
    s_srv->on("/__upload", HTTP_POST, handleUploadDone, handleUploadData);
    s_srv->onNotFound(handleRequest);
    s_srv->begin();
    io.out.printf("httpd: serving LittleFS at http://%s:%u/\n",
                  WiFi.localIP().toString().c_str(), port);
    io.out.println(F("       browse to download, use the form to upload, 'httpd stop' when done"));
    return 0;
  }

  if (sub == "stop") {
    if (!s_srv) { io.out.println(F("httpd: not running")); return 1; }
    s_srv->stop();
    delete s_srv;
    s_srv = nullptr;
    io.out.println(F("httpd: stopped"));
    return 0;
  }

  if (sub == "status") {
    if (!s_srv) { io.out.println(F("httpd: not running  (httpd start [port])")); return 0; }
    io.out.printf("httpd: running on http://%s:%u/  (%lu request(s) served)\n",
                  WiFi.localIP().toString().c_str(), s_port, (unsigned long)s_hits);
    return 0;
  }

  io.out.println(F("usage: httpd start [port] | stop | status"));
  return 1;
}

const Command HTTPD_CMDS[] = {
  {"httpd", cmd_httpd, "httpd start [port]|stop|status", "web file browser for LittleFS", G_NET},
};
const size_t HTTPD_CMDS_N = sizeof(HTTPD_CMDS) / sizeof(HTTPD_CMDS[0]);
