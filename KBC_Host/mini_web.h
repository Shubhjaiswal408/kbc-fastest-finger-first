// ============================================================================
//  MiniWeb - a tiny non-blocking HTTP/1.1 server.
//  ---------------------------------------------------------------------------
//  Why not the built-in WebServer? It serves one connection at a time, waits
//  up to 5 s for a silent connection, and blocks while a slow client reads its
//  reply. One weak phone then freezes the site for everyone.
//
//  MiniWeb:
//   * keeps up to MW_SLOTS connections side by side and drops silent ones;
//   * keeps connections alive, so a browser polling every 200 ms reuses one
//     connection instead of opening a new one each time;
//   * never waits on the network: replies are queued per connection and sent
//     with non-blocking writes, a little each pass.
//
//  Handlers use the WebServer-style API (arg, hasArg, send, sendHeader,
//  send_P, sendGzip, streamFile). send() only queues the reply, so it's cheap
//  to call while holding a lock.
// ============================================================================
#pragma once
#include <WiFi.h>
#include <FS.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <functional>
#include <memory>
#include <vector>

#define MW_SLOTS        12
#define MW_IDLE_MS      3000    // a new connection must send its request within this
#define MW_KEEPALIVE_MS 6000    // an idle kept-alive connection is closed after this
#define MW_STALL_MS     10000   // a client that stops reading its reply is dropped
#define MW_MAX_HEAD     4096
#define MW_MAX_BODY     65536

enum MiniMethod { MW_ANY = 0, MW_GET = 1, MW_POST = 2 };

typedef std::shared_ptr<const String> SharedBody;   // one reply body shared by many clients

class MiniWeb {
 public:
  typedef std::function<void()> Handler;

  explicit MiniWeb(uint16_t port) : _srv(port) {}

  void on(const char *uri, int method, Handler h) { _routes.push_back({String(uri), method, h}); }
  void onNotFound(Handler h) { _notFound = h; }
  void begin() { _srv.begin(); _srv.setNoDelay(true); }

  // Call often. Never waits for a client.
  void handleClient() {
    for (;;) {                                    // accept every waiting connection
      NetworkClient c = _srv.accept();
      if (!c) break;
      Slot &s = _s[_pickSlot()];
      s.c = c;
      s.used = true;
      s.since = millis();
    }
    for (int i = 0; i < MW_SLOTS; i++) {
      Slot &s = _s[i];
      if (!s.used) continue;
      if (_pending(s)) { _pump(s); continue; }     // still sending the last reply
      int n = s.c.available();
      while (n > 0 && s.buf.length() <= MW_MAX_HEAD + MW_MAX_BODY) {
        char tmp[512];
        int r = s.c.read((uint8_t *)tmp, min(n, (int)sizeof(tmp)));
        if (r <= 0) break;
        s.buf.concat(tmp, r);
        s.since = millis();
        n -= r;
      }
      if (_tryServe(s)) { _pump(s); continue; }
      uint32_t limit = (s.served && s.buf.length() == 0) ? MW_KEEPALIVE_MS : MW_IDLE_MS;
      if (!s.c.connected() || millis() - s.since > limit || s.buf.length() > MW_MAX_HEAD + MW_MAX_BODY)
        _close(s);
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
    if (!_begin(code, type, body.length())) return;
    if (!_cur->out.reserve(_cur->out.length() + body.length())) {   // out of memory: never send half a reply
      _cur->out = "HTTP/1.1 503 Busy\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      _cur->closeAfter = true;
      return;
    }
    _cur->out += body;
  }
  // The same body for many clients without copying it (e.g. the state JSON).
  void sendShared(int code, const char *type, SharedBody body) {
    if (!body || !_begin(code, type, body->length())) return;
    _cur->shared = body;
    _cur->ext = (const uint8_t *)body->c_str();
    _cur->extLen = body->length();
  }
  void send_P(int code, const char *type, const char *content) {
    size_t len = strlen(content);
    if (!_begin(code, type, len)) return;
    _cur->ext = (const uint8_t *)content;
    _cur->extLen = len;
  }
  // Pre-compressed content in flash (gzip), e.g. the embedded web page. Not copied.
  void sendGzip(int code, const char *type, const uint8_t *data, size_t len) {
    sendHeader("Content-Encoding", "gzip");
    if (!_begin(code, type, len)) return;
    _cur->ext = data;
    _cur->extLen = len;
  }
  void streamFile(File &f, const char *type) {
    size_t len = f.size();
    if (!_begin(200, type, len)) return;
    _cur->out.reserve(_cur->out.length() + len);
    uint8_t tmp[1024];
    while (f.available()) {
      int r = f.read(tmp, sizeof(tmp));
      if (r <= 0) break;
      _cur->out.concat((const char *)tmp, r);
    }
  }

 private:
  struct Slot {
    NetworkClient c;
    bool used = false, served = false, closeAfter = false;
    uint32_t since = 0;
    String buf;                         // request bytes received so far
    String out;                         // queued reply (head + body)
    size_t outOff = 0;
    const uint8_t *ext = nullptr;       // reply body sent without copying (flash or shared)
    size_t extLen = 0, extOff = 0;
    SharedBody shared;                  // keeps a shared body alive while sending
  };
  struct Route { String uri; int method; Handler h; };

  NetworkServer _srv;
  Slot _s[MW_SLOTS];
  std::vector<Route> _routes;
  Handler _notFound;
  // current request
  Slot *_cur = nullptr;
  bool _keepAlive = false;
  std::vector<std::pair<String, String>> _args;
  String _body, _hdrs;
  bool _sent = false;

  static bool _pending(const Slot &s) { return s.outOff < s.out.length() || (s.ext && s.extOff < s.extLen); }

  void _close(Slot &s) {
    s.c.stop();
    s.used = s.served = s.closeAfter = false;
    s.buf = String();
    s.out = String();
    s.outOff = s.extLen = s.extOff = 0;
    s.ext = nullptr;
    s.shared.reset();
  }

  // A free slot. When full, close (in this order): the oldest kept-alive connection
  // that is idle between requests (the browser just reconnects), a silent new one
  // older than 1 s, or the oldest of all. Never a connection that's mid-request.
  int _pickSlot() {
    int kept = -1, silent = -1, oldest = 0;
    uint32_t now = millis();
    for (int i = 0; i < MW_SLOTS; i++) {
      Slot &s = _s[i];
      if (!s.used) return i;
      bool idle = !_pending(s) && s.buf.length() == 0;
      if (idle && s.served && (kept < 0 || s.since < _s[kept].since)) kept = i;
      if (idle && !s.served && now - s.since > 1000 && (silent < 0 || s.since < _s[silent].since)) silent = i;
      if (s.since < _s[oldest].since) oldest = i;
    }
    int k = kept >= 0 ? kept : silent >= 0 ? silent : oldest;
    _close(_s[k]);
    return k;
  }

  // Send as much of the queued reply as the socket takes right now.
  void _pump(Slot &s) {
    int fd = s.c.fd();
    if (fd < 0) { _close(s); return; }
    while (_pending(s)) {
      const uint8_t *p;
      size_t n;
      bool head = s.outOff < s.out.length();
      if (head) { p = (const uint8_t *)s.out.c_str() + s.outOff; n = s.out.length() - s.outOff; }
      else      { p = s.ext + s.extOff; n = s.extLen - s.extOff; }
      if (n > 4096) n = 4096;
      int w = ::send(fd, p, n, MSG_DONTWAIT);
      if (w > 0) {
        if (head) s.outOff += w; else s.extOff += w;
        s.since = millis();
        continue;
      }
      if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {   // socket full: later
        if (millis() - s.since > MW_STALL_MS) _close(s);
        return;
      }
      _close(s);                                                   // client gone
      return;
    }
    // reply fully sent
    s.out = String();
    s.outOff = s.extLen = s.extOff = 0;
    s.ext = nullptr;
    s.shared.reset();
    if (s.closeAfter) { _close(s); return; }
    s.served = true;
    s.since = millis();
  }

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

  // Returns true if a complete request was found and a reply queued.
  bool _tryServe(Slot &s) {
    int he = s.buf.indexOf("\r\n\r\n");
    if (he < 0) return false;
    String head = s.buf.substring(0, he);
    int le = head.indexOf("\r\n");
    String line = le < 0 ? head : head.substring(0, le);
    int sp1 = line.indexOf(' '), sp2 = line.indexOf(' ', sp1 + 1);
    if (sp1 < 0 || sp2 < 0) { _close(s); return false; }
    String method = line.substring(0, sp1);
    String url = line.substring(sp1 + 1, sp2);
    bool http10 = line.endsWith("HTTP/1.0");

    String lower = head;
    lower.toLowerCase();
    size_t clen = 0;
    int ci = lower.indexOf("\r\ncontent-length:");
    if (ci >= 0) clen = lower.substring(ci + 17, lower.indexOf("\r\n", ci + 2)).toInt();
    bool form = lower.indexOf("application/x-www-form-urlencoded") >= 0;
    bool wantsClose = lower.indexOf("\r\nconnection: close") >= 0;
    bool wantsKeep = lower.indexOf("\r\nconnection: keep-alive") >= 0;

    _cur = &s;
    _hdrs = "";
    _sent = false;
    _keepAlive = http10 ? wantsKeep : !wantsClose;

    if (clen > MW_MAX_BODY) {                                        // too big: say so
      _keepAlive = false;
      send(413, "application/json", "{\"ok\":false,\"msg\":\"Too big\"}");
      s.buf = String();
      _cur = nullptr;
      return true;
    }
    if (s.buf.length() < (size_t)he + 4 + clen) { _cur = nullptr; return false; }   // body still coming

    _args.clear();
    _body = s.buf.substring(he + 4, he + 4 + clen);
    s.buf.remove(0, he + 4 + clen);                                  // keep any next request
    String path = url;
    int q = url.indexOf('?');
    if (q >= 0) { path = url.substring(0, q); _parseArgs(url.substring(q + 1)); }
    if (form) { _parseArgs(_body); _body = ""; }
    int m = method == "GET" ? MW_GET : method == "POST" ? MW_POST : MW_ANY;

    bool found = false;
    for (auto &r : _routes) {
      if (r.uri == path && (r.method == MW_ANY || r.method == m)) { r.h(); found = true; break; }
    }
    if (!found && _notFound) _notFound();
    if (!_sent) send(500, "text/plain", "no response");
    _body = String();
    _args.clear();
    _cur = nullptr;
    return true;
  }

  // Queue the status line + headers. False if there's no request or it was already answered.
  bool _begin(int code, const char *type, size_t len) {
    if (!_cur || _sent) return false;
    _sent = true;
    _cur->closeAfter = !_keepAlive;
    const char *txt = code == 200 ? "OK" : code == 302 ? "Found" : code == 400 ? "Bad Request" :
                      code == 404 ? "Not Found" : code == 413 ? "Payload Too Large" : "Error";
    String &o = _cur->out;
    o = "HTTP/1.1 ";
    o += code; o += ' '; o += txt;
    o += "\r\nContent-Type: "; o += type;
    o += "\r\nContent-Length: "; o += (unsigned)len;
    o += _keepAlive ? "\r\nConnection: keep-alive\r\nKeep-Alive: timeout=5\r\n" : "\r\nConnection: close\r\n";
    o += _hdrs;
    o += "\r\n";
    _cur->outOff = 0;
    return true;
  }
};
