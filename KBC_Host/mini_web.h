// ============================================================================
//  MiniWeb - a tiny HTTP/1.1 server that serves several connections at once.
//  ---------------------------------------------------------------------------
//  Why not the built-in WebServer? It handles one connection at a time and
//  waits up to 5 s for that connection to send its request. Chrome opens spare
//  "pre-connect" sockets that stay silent, so every one of them froze the
//  whole site for 5 s. MiniWeb keeps up to MW_SLOTS connections open side by
//  side and simply drops silent ones after MW_IDLE_MS.
//
//  The handler-side API mirrors WebServer (arg, hasArg, send, sendHeader,
//  send_P, streamFile) so handlers look the same.
// ============================================================================
#pragma once
#include <WiFi.h>
#include <FS.h>
#include <functional>
#include <vector>

#define MW_SLOTS     6
#define MW_IDLE_MS   3000     // drop a connection that sends nothing for this long
#define MW_MAX_HEAD  4096
#define MW_MAX_BODY  65536

enum MiniMethod { MW_ANY = 0, MW_GET = 1, MW_POST = 2 };

class MiniWeb {
 public:
  typedef std::function<void()> Handler;

  explicit MiniWeb(uint16_t port) : _srv(port) {}

  void on(const char *uri, int method, Handler h) { _routes.push_back({String(uri), method, h}); }
  void onNotFound(Handler h) { _notFound = h; }
  void begin() { _srv.begin(); _srv.setNoDelay(true); }

  // Call often. Never waits for a slow client.
  void handleClient() {
    // accept every waiting connection
    for (;;) {
      NetworkClient c = _srv.accept();
      if (!c) break;
      int free = -1, oldest = 0;
      for (int i = 0; i < MW_SLOTS; i++) {
        if (!_s[i].used) { free = i; break; }
        if (_s[i].since < _s[oldest].since) oldest = i;
      }
      if (free < 0) { _s[oldest].c.stop(); free = oldest; }   // full: drop the oldest
      _s[free].c = c;
      _s[free].used = true;
      _s[free].since = millis();
      _s[free].buf = "";
    }
    // serve the ones that have sent a full request
    for (int i = 0; i < MW_SLOTS; i++) {
      Slot &s = _s[i];
      if (!s.used) continue;
      int n = s.c.available();
      while (n > 0) {
        char tmp[512];
        int r = s.c.read((uint8_t *)tmp, min(n, (int)sizeof(tmp)));
        if (r <= 0) break;
        s.buf.concat(tmp, r);
        s.since = millis();
        n -= r;
        if (s.buf.length() > MW_MAX_HEAD + MW_MAX_BODY) break;
      }
      if (_tryServe(s)) continue;
      if (!s.c.connected() || millis() - s.since > MW_IDLE_MS || s.buf.length() > MW_MAX_HEAD + MW_MAX_BODY) {
        s.c.stop();
        s.used = false;
        s.buf = "";
      }
    }
  }

  // ---- for handlers ------------------------------------------------------
  bool hasArg(const String &name) const {
    if (name == "plain") return _body.length() > 0;
    for (auto &a : _args) if (a.first == name) return true;
    return false;
  }
  String arg(const String &name) const {
    if (name == "plain") return _body;
    for (auto &a : _args) if (a.first == name) return a.second;
    return String();
  }
  void sendHeader(const String &k, const String &v) { _hdrs += k + ": " + v + "\r\n"; }

  void send(int code, const char *type, const String &body) {
    _writeHead(code, type, body.length());
    _write((const uint8_t *)body.c_str(), body.length());
  }
  void send_P(int code, const char *type, const char *content) {
    size_t len = strlen(content);
    _writeHead(code, type, len);
    _write((const uint8_t *)content, len);
  }
  // Pre-compressed content (gzip), e.g. the embedded web page.
  void sendGzip(int code, const char *type, const uint8_t *data, size_t len) {
    sendHeader("Content-Encoding", "gzip");
    _writeHead(code, type, len);
    _write(data, len);
  }
  void streamFile(File &f, const char *type) {
    _writeHead(200, type, f.size());
    uint8_t tmp[1024];
    while (f.available()) {
      int r = f.read(tmp, sizeof(tmp));
      if (r <= 0) break;
      _write(tmp, r);
    }
  }

 private:
  struct Slot { NetworkClient c; bool used = false; uint32_t since = 0; String buf; };
  struct Route { String uri; int method; Handler h; };

  NetworkServer _srv;
  Slot _s[MW_SLOTS];
  std::vector<Route> _routes;
  Handler _notFound;
  // current request
  NetworkClient *_cur = nullptr;
  std::vector<std::pair<String, String>> _args;
  String _body, _hdrs;
  bool _sent = false;

  static String _decode(const String &in) {
    String o;
    o.reserve(in.length());
    for (size_t i = 0; i < in.length(); i++) {
      char c = in[i];
      if (c == '+') o += ' ';
      else if (c == '%' && i + 2 < in.length()) {
        char h[3] = {in[i + 1], in[i + 2], 0};
        o += (char)strtol(h, nullptr, 16);
        i += 2;
      } else o += c;
    }
    return o;
  }
  void _parseArgs(const String &s) {
    int start = 0;
    while (start < (int)s.length()) {
      int amp = s.indexOf('&', start);
      if (amp < 0) amp = s.length();
      String kv = s.substring(start, amp);
      int eq = kv.indexOf('=');
      if (kv.length()) _args.push_back({_decode(eq < 0 ? kv : kv.substring(0, eq)), eq < 0 ? String() : _decode(kv.substring(eq + 1))});
      start = amp + 1;
    }
  }

  // Returns true if the slot was served (and closed).
  bool _tryServe(Slot &s) {
    int he = s.buf.indexOf("\r\n\r\n");
    if (he < 0) return false;
    String head = s.buf.substring(0, he);
    int le = head.indexOf("\r\n");
    String line = le < 0 ? head : head.substring(0, le);
    int sp1 = line.indexOf(' '), sp2 = line.indexOf(' ', sp1 + 1);
    if (sp1 < 0 || sp2 < 0) { s.c.stop(); s.used = false; return true; }
    String method = line.substring(0, sp1);
    String url = line.substring(sp1 + 1, sp2);

    size_t clen = 0;
    bool form = false;
    String lower = head;
    lower.toLowerCase();
    int ci = lower.indexOf("\r\ncontent-length:");
    if (ci >= 0) clen = lower.substring(ci + 17, lower.indexOf("\r\n", ci + 2)).toInt();
    if (lower.indexOf("application/x-www-form-urlencoded") >= 0) form = true;
    if (clen > MW_MAX_BODY) { s.c.stop(); s.used = false; return true; }
    if (s.buf.length() < (size_t)he + 4 + clen) return false;           // body still coming

    _args.clear();
    _body = s.buf.substring(he + 4, he + 4 + clen);
    _hdrs = "";
    _sent = false;
    String path = url;
    int q = url.indexOf('?');
    if (q >= 0) { path = url.substring(0, q); _parseArgs(url.substring(q + 1)); }
    if (form) { _parseArgs(_body); _body = ""; }
    int m = method == "GET" ? MW_GET : method == "POST" ? MW_POST : MW_ANY;

    _cur = &s.c;
    bool found = false;
    for (auto &r : _routes) {
      if (r.uri == path && (r.method == MW_ANY || r.method == m)) { r.h(); found = true; break; }
    }
    if (!found && _notFound) _notFound();
    if (!_sent) send(500, "text/plain", "no response");
    s.c.flush();
    s.c.stop();
    s.used = false;
    s.buf = "";
    _cur = nullptr;
    return true;
  }

  void _writeHead(int code, const char *type, size_t len) {
    if (!_cur || _sent) return;
    _sent = true;
    const char *txt = code == 200 ? "OK" : code == 302 ? "Found" : code == 400 ? "Bad Request" : code == 404 ? "Not Found" : "Error";
    String h = "HTTP/1.1 " + String(code) + " " + txt + "\r\nContent-Type: " + type +
               "\r\nContent-Length: " + String(len) + "\r\nConnection: close\r\n" + _hdrs + "\r\n";
    _write((const uint8_t *)h.c_str(), h.length());
  }
  void _write(const uint8_t *p, size_t len) {
    if (!_cur) return;
    uint32_t t0 = millis();
    while (len > 0 && _cur->connected() && millis() - t0 < 8000) {
      size_t w = _cur->write(p, min(len, (size_t)2048));
      if (w == 0) { delay(2); continue; }
      p += w;
      len -= w;
    }
  }
};
