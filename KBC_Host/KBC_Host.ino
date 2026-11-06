// ============================================================================
//  KBC Buzzer - HOST firmware (ESP32)
//  ---------------------------------------------------------------------------
//  * Creates its own Wi-Fi network (default "KBC-Quiz" / "kbc12345")
//  * Serves the quiz web app at http://192.168.4.1  (or http://kbc.local)
//  * Talks to every student buzzer over ESP-NOW (no router needed)
//  * Everything (names, rules, points, Wi-Fi, questions) is set from the web app
//    and saved on the host.
//
//  Board: "ESP32 Dev Module" (esp32 core 3.x), default partition scheme.
//  Only built-in libraries are used.
// ============================================================================
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include "mini_web.h"
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <stdarg.h>
#include "protocol.h"
#include "web_index.h"
// Optional: your own Wi-Fi for the host to join (kept out of git). See wifi_secrets.example.h
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif
#ifndef HOME_WIFI_SSID
#define HOME_WIFI_SSID ""
#define HOME_WIFI_PASS ""
#endif

#define FW_VERSION_STR "1.3.0"
#define LED_PIN        2
#define MAX_PLAYERS    40
#define ONLINE_MS      3500

// Defined early: Arduino inserts function prototypes above the first function.
struct RxItem {
  uint8_t mac[6];
  int8_t  rssi;
  int64_t atUs;
  uint8_t len;
  uint8_t data[250];    // ESP-NOW max payload (log lines need the room)
};

static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint32_t PALETTE[] = {0xF5B700, 0x00B4D8, 0xFF5D8F, 0x7CD83E, 0xB388FF, 0xFF8A3D,
                                   0x2EC4B6, 0xF15BB5, 0x9BC53D, 0x5E9BFF, 0xFFD166, 0xEF476F};

// ---------------------------------------------------------------------------
//  Settings (persisted in NVS)
// ---------------------------------------------------------------------------
struct Settings {
  uint8_t  mode = 0;          // 0 = fastest correct answer wins, 1 = buzzer (first press wins)
  uint16_t countdownS = 3;    // countdown before answering opens
  uint16_t limitS = 20;       // default answer time (0 = no limit)
  bool     autoLock = true;   // lock when every online buzzer answered
  bool     falseStart = true; // pressing during countdown locks you out
  bool     showQcd = false;   // show question during countdown
  int16_t  ptsWin = 10;
  int16_t  ptsCorrect = 0;
  int16_t  ptsWrong = 0;
  bool     acceptNew = true;  // accept new buzzers
  String   title = "Fastest Finger First";
  // network
  String   apSsid = "KBC-Quiz";
  String   apPass = "kbc12345";
  uint8_t  apCh = 1;
  String   staSsid = "";
  String   staPass = "";
  String   hostName = "kbc";
} S;

// ---------------------------------------------------------------------------
//  Players
// ---------------------------------------------------------------------------
enum { RES_NONE = 0, RES_WRONG = 1, RES_CORRECT = 2, RES_WINNER = 3, RES_FALSE = 4 };
enum { AN_NONE = 0, AN_OK = 1, AN_FALSE = 2 };

struct PlayerSaved {
  uint8_t mac[6];
  char    name[24];
  uint32_t color;
  int32_t score;
  uint8_t no;
};

struct Player {
  bool     used = false;
  PlayerSaved d;
  uint32_t lastSeen = 0;
  int8_t   rssi = 0;
  uint16_t fw = 0;
  bool     battery = false;   // sleeps between presses (XIAO C6 battery buzzer)
  // current round
  uint8_t  ans = AN_NONE;
  uint8_t  choice = 0xFF;
  uint32_t reactUs = 0;
  uint32_t hostUs = 0;
  uint8_t  result = RES_NONE;
  int16_t  pts = 0;
  // button test (live from the buzzer)
  uint8_t  btnHeld = 0;
  uint16_t btnPresses[4] = {0, 0, 0, 0};
  uint16_t btnBase[4] = {0, 0, 0, 0};    // "reset counters" offsets
  uint16_t btnGlitches = 0;
  uint16_t glitchBase = 0;
};
static Player players[MAX_PLAYERS];

// ---------------------------------------------------------------------------
//  Log viewer: one ring buffer with lines from the host and every buzzer
// ---------------------------------------------------------------------------
#define LOG_LINES 220
#define LOG_LINE_LEN 160
struct LogLine {
  uint32_t seq;
  uint32_t ms;
  uint8_t  mac[6];      // all zero = host
  char     text[LOG_LINE_LEN];
};
static LogLine logRing[LOG_LINES];
static uint32_t logSeq = 0;

// The game + radio run in their own task so a slow web client can never pause
// them. Everything they share with the web handlers is guarded by this lock.
static SemaphoreHandle_t gameMtx = nullptr;
struct GameLock {
  GameLock()  { if (gameMtx) xSemaphoreTakeRecursive(gameMtx, portMAX_DELAY); }
  ~GameLock() { if (gameMtx) xSemaphoreGiveRecursive(gameMtx); }
};

static void addLog(const uint8_t *mac, const char *text, size_t len) {
  GameLock g;
  LogLine &l = logRing[logSeq % LOG_LINES];
  l.seq = ++logSeq;
  l.ms = millis();
  if (mac) memcpy(l.mac, mac, 6); else memset(l.mac, 0, 6);
  if (len >= LOG_LINE_LEN) len = LOG_LINE_LEN - 1;
  memcpy(l.text, text, len);
  l.text[len] = 0;
}

// Print to Serial AND keep the line for the web app's Logs tab.
static void hlogf(const char *fmt, ...) {
  char line[LOG_LINE_LEN];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  Serial.println(line);
  addLog(nullptr, line, strlen(line));
}

// ---------------------------------------------------------------------------
//  Round
// ---------------------------------------------------------------------------
static uint16_t roundId;
static uint8_t  phase = PH_IDLE;
static int64_t  cdEndUs = 0;
static int64_t  openAtUs = 0;
static int64_t  lockAtUs = 0;
static uint32_t limitMs = 0;
static uint8_t  correct = 0xFF;      // kept secret until reveal
static int      winner = -1;
static String   qText;
static String   qOpt[4];
static int      qIndex = -1;
static uint32_t stateVersion = 1;
// Reveal waits this long after the lock, so presses made just before the lock
// (still in the air, ~0.1 s for a battery buzzer) are counted first.
#define REVEAL_GRACE_US 300000
#define LATE_PRESS_US   250000      // USB-buzzer press arriving just after the lock
static bool     revealPending = false;
static uint8_t  revealAns = 0xFF;

// ---------------------------------------------------------------------------
static Preferences prefs;
static MiniWeb server(80);
static QueueHandle_t rxQ;
static bool restartPending = false;
static uint32_t restartAt = 0;
static uint32_t lastStateTx = 0;
static uint8_t  burst = 0;

// ============================================================================
//  Utilities
// ============================================================================
static void bump() { stateVersion++; }

static void macToStr(const uint8_t *m, char *out) {
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static bool strToMac(const String &s, uint8_t *m) {
  unsigned v[6];
  if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) m[i] = (uint8_t)v[i];
  return true;
}

static void jsonStr(String &o, const char *s) {
  o += '"';
  for (; *s; s++) {
    char c = *s;
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
    else o += c;
  }
  o += '"';
}
static void jsonStr(String &o, const String &s) { jsonStr(o, s.c_str()); }

static void fillHeader(MsgHeader &h, uint8_t type) {
  h.magic = KBC_MAGIC;
  h.proto = KBC_PROTO;
  h.type = type;
}

static Player *findPlayer(const uint8_t *mac) {
  for (auto &p : players) if (p.used && memcmp(p.d.mac, mac, 6) == 0) return &p;
  return nullptr;
}

// Battery buzzers sleep and check in once an hour, so they count as "ready" for longer.
#define BATTERY_READY_MS (75UL * 60UL * 1000UL)
static bool isAwake(const Player &p) { return p.lastSeen && millis() - p.lastSeen < ONLINE_MS; }
static bool isOnline(const Player &p) {
  return p.lastSeen && millis() - p.lastSeen < (p.battery ? BATTERY_READY_MS : ONLINE_MS);
}

// ============================================================================
//  Persistence
// ============================================================================
// Flash writes stall BOTH cores for 10-100 ms, so player saves are deferred:
// savePlayers() only marks them dirty, and loop() writes them a moment later,
// outside the game lock and never in the middle of a press.
static volatile bool playersDirty = false;
static volatile uint32_t playersDirtyAt = 0;
static void savePlayers() { playersDirty = true; playersDirtyAt = millis(); }

static void flushPlayers() {
  if (!playersDirty) return;
  static PlayerSaved buf[MAX_PLAYERS];
  size_t n = 0;
  {
    GameLock g;
    for (auto &p : players) if (p.used) buf[n++] = p.d;
    playersDirty = false;
  }
  prefs.putBytes("players", buf, n * sizeof(PlayerSaved));
}

static void loadPlayers() {
  PlayerSaved buf[MAX_PLAYERS];
  size_t len = prefs.getBytesLength("players");
  if (len == 0 || len % sizeof(PlayerSaved) != 0 || len > sizeof(buf)) return;
  prefs.getBytes("players", buf, len);
  size_t n = len / sizeof(PlayerSaved);
  for (size_t i = 0; i < n; i++) {
    players[i].used = true;
    players[i].d = buf[i];
    players[i].d.name[sizeof(players[i].d.name) - 1] = 0;
  }
}

static void saveSettings() {
  prefs.putUChar("mode", S.mode);
  prefs.putUShort("cd", S.countdownS);
  prefs.putUShort("lim", S.limitS);
  prefs.putBool("auto", S.autoLock);
  prefs.putBool("fs", S.falseStart);
  prefs.putBool("sq", S.showQcd);
  prefs.putShort("pw", S.ptsWin);
  prefs.putShort("pc", S.ptsCorrect);
  prefs.putShort("px", S.ptsWrong);
  prefs.putBool("acc", S.acceptNew);
  prefs.putString("title", S.title);
  prefs.putString("ap", S.apSsid);
  prefs.putString("appw", S.apPass);
  prefs.putUChar("ch", S.apCh);
  prefs.putString("sta", S.staSsid);
  prefs.putString("stapw", S.staPass);
  prefs.putString("host", S.hostName);
}

static void loadSettings() {
  S.mode = prefs.getUChar("mode", S.mode);
  S.countdownS = prefs.getUShort("cd", S.countdownS);
  S.limitS = prefs.getUShort("lim", S.limitS);
  S.autoLock = prefs.getBool("auto", S.autoLock);
  S.falseStart = prefs.getBool("fs", S.falseStart);
  S.showQcd = prefs.getBool("sq", S.showQcd);
  S.ptsWin = prefs.getShort("pw", S.ptsWin);
  S.ptsCorrect = prefs.getShort("pc", S.ptsCorrect);
  S.ptsWrong = prefs.getShort("px", S.ptsWrong);
  S.acceptNew = prefs.getBool("acc", S.acceptNew);
  S.title = prefs.getString("title", S.title);
  S.apSsid = prefs.getString("ap", S.apSsid);
  S.apPass = prefs.getString("appw", S.apPass);
  S.apCh = prefs.getUChar("ch", S.apCh);
  S.staSsid = prefs.getString("sta", S.staSsid);
  S.staPass = prefs.getString("stapw", S.staPass);
  S.hostName = prefs.getString("host", S.hostName);
  if (S.apCh < 1 || S.apCh > 13) S.apCh = 1;
  if (S.staSsid.length() == 0 && strlen(HOME_WIFI_SSID) > 0) {   // fall back to wifi_secrets.h
    S.staSsid = HOME_WIFI_SSID;
    S.staPass = HOME_WIFI_PASS;
  }
}

// ============================================================================
//  Players management
// ============================================================================
static Player *registerPlayer(const uint8_t *mac) {
  Player *p = findPlayer(mac);
  if (p) return p;
  if (!S.acceptNew) return nullptr;
  uint8_t usedNo[MAX_PLAYERS + 2] = {0};
  int count = 0;
  for (auto &q : players) if (q.used) { count++; if (q.d.no <= MAX_PLAYERS) usedNo[q.d.no] = 1; }
  for (auto &q : players) {
    if (q.used) continue;
    q = Player();
    q.used = true;
    memcpy(q.d.mac, mac, 6);
    uint8_t no = 1;
    while (no <= MAX_PLAYERS && usedNo[no]) no++;
    q.d.no = no;
    snprintf(q.d.name, sizeof(q.d.name), "Player %u", no);
    q.d.color = PALETTE[(no - 1) % (sizeof(PALETTE) / sizeof(PALETTE[0]))];
    q.d.score = 0;
    savePlayers();
    char s[18];
    macToStr(mac, s);
    hlogf("[players] new buzzer %s -> %s", s, q.d.name);
    bump();
    return &q;
  }
  return nullptr;  // full
}

// ============================================================================
//  Radio
// ============================================================================
static void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(MsgHeader) || len > (int)sizeof(RxItem::data)) return;
  if (data[0] != KBC_MAGIC || data[1] != KBC_PROTO) return;
  RxItem it;
  it.atUs = esp_timer_get_time();
  memcpy(it.mac, info->src_addr, 6);
  it.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
  it.len = (uint8_t)len;
  memcpy(it.data, data, len);
  xQueueSend(rxQ, &it, 0);
}

static void sendState() {
  StateMsg m;
  memset(&m, 0, sizeof(m));
  fillHeader(m.h, MSG_STATE);
  m.roundId = roundId;
  m.phase = phase;
  m.correct = (phase == PH_REVEAL) ? correct : 0xFF;
  int64_t now = esp_timer_get_time();
  if (phase == PH_OPEN) m.elapsedUs = (uint32_t)(now - openAtUs);
  else if (phase == PH_LOCKED || phase == PH_REVEAL) m.elapsedUs = (uint32_t)(lockAtUs - openAtUs);
  if (phase == PH_COUNTDOWN) m.countdownMs = (uint32_t)max<int64_t>(0, (cdEndUs - now) / 1000);
  m.limitMs = limitMs;
  if (phase == PH_REVEAL && winner >= 0) memcpy(m.winner, players[winner].d.mac, 6);
  esp_now_send(BCAST, (uint8_t *)&m, sizeof(m));
  lastStateTx = millis();
}

static void sendAck(const uint8_t *mac, uint8_t status) {
  PressAckMsg m;
  fillHeader(m.h, MSG_PRESS_ACK);
  memcpy(m.to, mac, 6);
  m.roundId = roundId;
  m.status = status;
  esp_now_send(BCAST, (uint8_t *)&m, sizeof(m));
}

static void sendWelcome(const uint8_t *mac, Player *p) {
  WelcomeMsg m;
  fillHeader(m.h, MSG_WELCOME);
  memcpy(m.to, mac, 6);
  m.accepted = p ? 1 : 0;
  m.playerNo = p ? p->d.no : 0;
  m.channel = WiFi.channel();
  esp_now_send(BCAST, (uint8_t *)&m, sizeof(m));
}

static void sendIdentify(const uint8_t *mac) {
  IdentifyMsg m;
  fillHeader(m.h, MSG_IDENTIFY);
  memcpy(m.to, mac, 6);
  m.ms = 3000;
  for (int i = 0; i < 3; i++) esp_now_send(BCAST, (uint8_t *)&m, sizeof(m));   // queued by the radio, no wait
}

static void setPhase(uint8_t ph) {
  phase = ph;
  burst = 3;          // send the new state a few times quickly
  sendState();
  bump();
}

// ============================================================================
//  Round logic
// ============================================================================
static void lockRound() {
  if (phase != PH_OPEN && phase != PH_COUNTDOWN) return;
  lockAtUs = esp_timer_get_time();
  if (phase == PH_COUNTDOWN) openAtUs = lockAtUs;
  setPhase(PH_LOCKED);
}

static bool fasterThan(const Player &a, const Player &b) {
  if (a.reactUs != b.reactUs) return a.reactUs < b.reactUs;
  return a.hostUs < b.hostUs;
}

static void revealRound(uint8_t ans) {
  if (phase == PH_OPEN || phase == PH_COUNTDOWN) lockRound();
  if (phase != PH_LOCKED) return;
  correct = ans;
  winner = -1;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    Player &p = players[i];
    if (!p.used) continue;
    p.result = RES_NONE;
    p.pts = 0;
    if (p.ans == AN_FALSE) { p.result = RES_FALSE; continue; }
    if (p.ans != AN_OK) continue;
    if (S.mode == 1) {                                       // buzzer mode
      if (winner < 0 || fasterThan(p, players[winner])) winner = i;
      continue;
    }
    if (p.choice == correct) {
      p.result = RES_CORRECT;
      if (winner < 0 || fasterThan(p, players[winner])) winner = i;
    } else {
      p.result = RES_WRONG;
    }
  }
  if (winner >= 0) players[winner].result = RES_WINNER;

  if (S.mode == 0) {                                         // auto points
    bool changed = false;
    for (auto &p : players) {
      if (!p.used) continue;
      if (p.result == RES_WINNER) p.pts = S.ptsWin;
      else if (p.result == RES_CORRECT) p.pts = S.ptsCorrect;
      else if (p.result == RES_WRONG || p.result == RES_FALSE) p.pts = S.ptsWrong;
      if (p.pts) { p.d.score += p.pts; changed = true; }
    }
    if (changed) savePlayers();
  }
  setPhase(PH_REVEAL);
  if (winner >= 0) hlogf("[round %u] winner: %s (%.3f s)", roundId, players[winner].d.name, players[winner].reactUs / 1e6);
  else hlogf("[round %u] revealed, no winner", roundId);
}

static void clearAnswers() {
  revealPending = false;
  for (auto &p : players) {
    p.ans = AN_NONE; p.choice = 0xFF; p.reactUs = 0; p.hostUs = 0; p.result = RES_NONE; p.pts = 0;
  }
  winner = -1;
}

static void startRound(uint32_t limMs, uint8_t ans) {
  roundId++;
  if (roundId == 0) roundId = 1;
  clearAnswers();
  correct = ans;
  limitMs = limMs;
  int64_t now = esp_timer_get_time();
  if (S.countdownS > 0) {
    cdEndUs = now + (int64_t)S.countdownS * 1000000LL;
    setPhase(PH_COUNTDOWN);
  } else {
    openAtUs = now;
    setPhase(PH_OPEN);
  }
  hlogf("[round %u] started", roundId);
}

static void resetRound() {
  clearAnswers();
  roundId++;
  qText = ""; for (auto &o : qOpt) o = "";
  qIndex = -1;
  correct = 0xFF;
  setPhase(PH_IDLE);
}

static void tickRound() {
  int64_t now = esp_timer_get_time();
  if (revealPending) {
    if (phase != PH_LOCKED) revealPending = false;
    else if (now - lockAtUs >= REVEAL_GRACE_US) { revealPending = false; revealRound(revealAns); }
  }
  if (phase == PH_COUNTDOWN && now >= cdEndUs) {
    openAtUs = now;
    setPhase(PH_OPEN);
  }
  if (phase == PH_OPEN) {
    if (limitMs && now - openAtUs >= (int64_t)limitMs * 1000) { lockRound(); return; }
    if (S.autoLock) {
      int online = 0, done = 0;
      for (auto &p : players) {
        if (!p.used || !isOnline(p)) continue;
        online++;
        if (p.ans != AN_NONE) done++;
      }
      if (online > 0 && done >= online) lockRound();
    }
  }
}

static void handlePress(const RxItem &it, Player *p) {
  const PressMsg *m = (const PressMsg *)it.data;
  if (it.len < sizeof(PressMsg)) return;
  if (m->roundId != roundId) { sendAck(it.mac, ACK_CLOSED); return; }
  if (p->ans != AN_NONE) { sendAck(it.mac, p->ans == AN_FALSE ? ACK_FALSE_START : ACK_DUPLICATE); return; }

  // Pressed before the lock but arrived just after it -> still counts (like battery buzzers).
  bool late = phase == PH_LOCKED && !m->early && it.atUs - lockAtUs < LATE_PRESS_US &&
              m->reactionUs <= (uint32_t)(lockAtUs - openAtUs);
  int64_t hostElapsed = (phase == PH_OPEN || late) ? it.atUs - openAtUs : -1;
  bool tooEarly = phase == PH_COUNTDOWN || (phase == PH_OPEN && m->early && hostElapsed < 150000);

  if (tooEarly) {
    if (S.falseStart) {
      p->ans = AN_FALSE;
      p->choice = m->choice;
      sendAck(it.mac, ACK_FALSE_START);
      bump();
    } else {
      sendAck(it.mac, ACK_IGNORED);
    }
    return;
  }
  if (phase != PH_OPEN && !late) { sendAck(it.mac, ACK_CLOSED); return; }

  // Question with only 2 or 3 options: a button for a missing option does nothing,
  // so the student can still press a real one.
  if (qText.length() && qOpt[m->choice & 3].length() == 0) {
    sendAck(it.mac, ACK_IGNORED);
    hlogf("[press] %s pressed %c - not an option, ignored", p->d.name, 'A' + (m->choice & 3));
    return;
  }

  uint32_t r = m->early ? (uint32_t)hostElapsed : m->reactionUs;
  if (r > (uint32_t)hostElapsed) r = (uint32_t)hostElapsed;   // can never be slower than the host saw
  p->ans = AN_OK;
  p->choice = m->choice & 3;
  p->reactUs = r;
  p->hostUs = (uint32_t)hostElapsed;
  sendAck(it.mac, ACK_ACCEPTED);
  bump();
  hlogf("[press] %s -> %c  %.3f s", p->d.name, 'A' + p->choice, r / 1e6);
  if (S.mode == 1) lockRound();                  // buzzer mode: first press locks
}

// A battery buzzer was asleep: it reports how long ago the button was pressed,
// so we know the real press moment even though the packet arrives ~0.1 s later.
static void handleBPress(const RxItem &it, Player *p) {
  if (it.len < sizeof(BPressMsg)) return;
  const BPressMsg *m = (const BPressMsg *)it.data;
  uint8_t choice = m->choice & 3;
  int64_t pressAt = it.atUs - (int64_t)m->agoUs;

  if (p->ans != AN_NONE) { sendAck(it.mac, p->ans == AN_FALSE ? ACK_FALSE_START : ACK_DUPLICATE); return; }
  if (phase == PH_IDLE || phase == PH_REVEAL) { sendAck(it.mac, ACK_CLOSED); return; }

  bool duringCountdown = phase == PH_COUNTDOWN || pressAt < openAtUs;
  if (duringCountdown) {
    if (S.falseStart) {
      p->ans = AN_FALSE;
      p->choice = choice;
      sendAck(it.mac, ACK_FALSE_START);
      bump();
      hlogf("[press] %s -> FALSE START (battery buzzer)", p->d.name);
    } else {
      sendAck(it.mac, ACK_IGNORED);
    }
    return;
  }
  // Pressed before the round locked but arrived just after -> still counts.
  if (phase == PH_LOCKED && pressAt > lockAtUs) { sendAck(it.mac, ACK_CLOSED); return; }

  if (qText.length() && qOpt[choice].length() == 0) {
    sendAck(it.mac, ACK_IGNORED);
    hlogf("[press] %s pressed %c - not an option, ignored", p->d.name, 'A' + choice);
    return;
  }

  p->ans = AN_OK;
  p->choice = choice;
  p->reactUs = (uint32_t)(pressAt - openAtUs);
  p->hostUs = (uint32_t)(it.atUs - openAtUs);
  sendAck(it.mac, ACK_ACCEPTED);
  bump();
  hlogf("[press] %s -> %c  %.3f s (battery, arrived %lu ms after the press)", p->d.name, 'A' + choice,
        p->reactUs / 1e6, (unsigned long)(m->agoUs / 1000));
  if (S.mode == 1 && phase == PH_OPEN) lockRound();
}

static void handleRx(const RxItem &it) {
  {
    const MsgHeader *h = (const MsgHeader *)it.data;
    Player *p = nullptr;
    switch (h->type) {
      case MSG_HELLO: {
        p = registerPlayer(it.mac);
        if (p && it.len >= sizeof(HelloMsg)) {
          const HelloMsg *hm = (const HelloMsg *)it.data;
          p->fw = hm->fw;
          p->battery = (hm->buttons & HELLO_BATTERY_FLAG) != 0;
        }
        sendWelcome(it.mac, p);
        break;
      }
      case MSG_BPRESS:
        p = registerPlayer(it.mac);
        if (p) { p->battery = true; handleBPress(it, p); }
        break;
      case MSG_PING:
        p = registerPlayer(it.mac);
        break;
      case MSG_PRESS:
        p = registerPlayer(it.mac);
        if (p) handlePress(it, p);
        break;
      case MSG_BUTTONS:
        p = registerPlayer(it.mac);
        if (p && it.len >= sizeof(ButtonsMsg)) {
          const ButtonsMsg *m = (const ButtonsMsg *)it.data;
          bool changed = p->btnHeld != m->held || p->btnGlitches != m->glitches;
          for (int i = 0; i < 4; i++) {
            if (m->presses[i] < p->btnPresses[i]) p->btnBase[i] = 0;   // buzzer rebooted
            if (p->btnPresses[i] != m->presses[i]) changed = true;
            p->btnPresses[i] = m->presses[i];
          }
          if (m->glitches < p->btnGlitches) p->glitchBase = 0;
          p->btnHeld = m->held;
          p->btnGlitches = m->glitches;
          if (changed) bump();
        }
        break;
      case MSG_LOG:
        p = findPlayer(it.mac);
        if (it.len > sizeof(MsgHeader) + 2) {
          const LogMsg *m = (const LogMsg *)it.data;
          addLog(it.mac, m->text, it.len - sizeof(MsgHeader) - 2);
        }
        break;
      default:
        break;
    }
    if (p) {
      bool wasOnline = isOnline(*p);
      p->lastSeen = millis();
      if (p->lastSeen == 0) p->lastSeen = 1;
      p->rssi = it.rssi;
      if (!wasOnline) bump();
    }
  }
}

// ============================================================================
//  Web API
// ============================================================================
static String stateJson() {
  String o;
  o.reserve(2048 + 180 * MAX_PLAYERS);
  int64_t now = esp_timer_get_time();
  o += "{\"v\":"; o += stateVersion;
  o += ",\"fw\":\"" FW_VERSION_STR "\"";
  o += ",\"up\":"; o += millis() / 1000;
  o += ",\"heap\":"; o += ESP.getFreeHeap();
  o += ",\"phase\":"; o += phase;
  o += ",\"round\":"; o += roundId;
  o += ",\"cdLeft\":"; o += (phase == PH_COUNTDOWN) ? (long)max<int64_t>(0, (cdEndUs - now) / 1000) : 0;
  long elapsed = 0;
  if (phase == PH_OPEN) elapsed = (now - openAtUs) / 1000;
  else if (phase == PH_LOCKED || phase == PH_REVEAL) elapsed = (lockAtUs - openAtUs) / 1000;
  o += ",\"elapsed\":"; o += elapsed;
  o += ",\"limit\":"; o += limitMs;
  o += ",\"correct\":"; o += (phase == PH_REVEAL && correct != 0xFF) ? (int)correct : -1;
  o += ",\"winner\":";
  if (phase == PH_REVEAL && winner >= 0) { char s[18]; macToStr(players[winner].d.mac, s); jsonStr(o, s); } else o += "null";
  bool showQ = phase >= PH_OPEN || (phase == PH_COUNTDOWN && S.showQcd);
  o += ",\"q\":{\"i\":"; o += qIndex;
  o += ",\"has\":"; o += qText.length() ? 1 : 0;
  o += ",\"t\":"; jsonStr(o, showQ ? qText : String(""));
  o += ",\"o\":[";
  for (int i = 0; i < 4; i++) { if (i) o += ','; jsonStr(o, showQ ? qOpt[i] : String("")); }
  o += "]}";

  o += ",\"players\":[";
  bool first = true;
  bool hideChoice = phase == PH_COUNTDOWN || phase == PH_OPEN;
  for (auto &p : players) {
    if (!p.used) continue;
    if (!first) o += ',';
    first = false;
    char mac[18]; macToStr(p.d.mac, mac);
    char col[8]; snprintf(col, sizeof(col), "#%06X", (unsigned)(p.d.color & 0xFFFFFF));
    o += "{\"m\":\""; o += mac; o += "\"";
    o += ",\"no\":"; o += p.d.no;
    o += ",\"n\":"; jsonStr(o, p.d.name);
    o += ",\"c\":\""; o += col; o += "\"";
    o += ",\"s\":"; o += p.d.score;
    o += ",\"on\":"; o += isOnline(p) ? 1 : 0;
    o += ",\"r\":"; o += p.rssi;
    o += ",\"seen\":"; o += p.lastSeen ? (long)(millis() - p.lastSeen) : -1;
    o += ",\"fw\":"; o += p.fw;
    o += ",\"bat\":"; o += p.battery ? 1 : 0;
    o += ",\"awake\":"; o += isAwake(p) ? 1 : 0;
    o += ",\"b\":{\"k\":"; o += p.btnHeld;
    o += ",\"p\":[";
    for (int i = 0; i < 4; i++) { if (i) o += ','; o += (uint16_t)(p.btnPresses[i] - p.btnBase[i]); }
    o += "],\"g\":"; o += (uint16_t)(p.btnGlitches - p.glitchBase);
    o += "}";
    o += ",\"a\":";
    if (p.ans == AN_NONE) o += "null";
    else {
      o += "{\"st\":"; o += p.ans;
      o += ",\"c\":"; o += hideChoice ? -1 : (int)p.choice;
      o += ",\"t\":"; o += p.reactUs;
      o += ",\"res\":"; o += p.result;
      o += ",\"pts\":"; o += p.pts;
      o += "}";
    }
    o += "}";
  }
  o += "]";

  o += ",\"set\":{\"mode\":"; o += S.mode;
  o += ",\"cd\":"; o += S.countdownS;
  o += ",\"lim\":"; o += S.limitS;
  o += ",\"auto\":"; o += S.autoLock ? 1 : 0;
  o += ",\"fs\":"; o += S.falseStart ? 1 : 0;
  o += ",\"sq\":"; o += S.showQcd ? 1 : 0;
  o += ",\"pw\":"; o += S.ptsWin;
  o += ",\"pc\":"; o += S.ptsCorrect;
  o += ",\"px\":"; o += S.ptsWrong;
  o += ",\"acc\":"; o += S.acceptNew ? 1 : 0;
  o += ",\"title\":"; jsonStr(o, S.title);
  o += "}";

  o += ",\"net\":{\"ap\":"; jsonStr(o, S.apSsid);
  o += ",\"apIp\":\""; o += WiFi.softAPIP().toString(); o += "\"";
  o += ",\"ch\":"; o += WiFi.channel();
  o += ",\"apCh\":"; o += S.apCh;
  o += ",\"sta\":"; jsonStr(o, S.staSsid);
  o += ",\"staIp\":\""; o += (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String(""); o += "\"";
  o += ",\"host\":"; jsonStr(o, S.hostName);
  o += ",\"clients\":"; o += WiFi.softAPgetStationNum();
  o += "}}";
  return o;
}

static void ok(const char *msg = "ok") {
  String o = "{\"ok\":true,\"msg\":"; jsonStr(o, msg); o += "}";
  server.send(200, "application/json", o);
}
static void fail(const char *msg, int code = 400) {
  String o = "{\"ok\":false,\"msg\":"; jsonStr(o, msg); o += "}";
  server.send(code, "application/json", o);
}

static Player *argPlayer() {
  uint8_t mac[6];
  if (!strToMac(server.arg("m"), mac)) return nullptr;
  return findPlayer(mac);
}

// API handler that runs while holding the game lock (small replies only).
static void onApi(const char *uri, int method, std::function<void()> fn) {
  server.on(uri, method, [fn]() { GameLock g; fn(); });
}

static void setupWeb() {
  server.on("/", MW_GET, []() {
    server.sendHeader("Cache-Control", "no-cache");
    server.sendGzip(200, "text/html; charset=utf-8", INDEX_HTML_GZ, INDEX_HTML_GZ_LEN);
  });

  server.on("/api/state", MW_GET, []() {
    String s;
    { GameLock g; s = stateJson(); }          // build under the lock, send without it
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", s);
  });

  onApi("/api/start", MW_POST, []() {
    if (phase == PH_COUNTDOWN || phase == PH_OPEN) return fail("A round is already running");
    qText = server.arg("q").substring(0, 300);
    for (int i = 0; i < 4; i++) qOpt[i] = server.arg("o" + String(i)).substring(0, 120);
    qIndex = server.hasArg("qi") ? server.arg("qi").toInt() : -1;
    long lim = server.hasArg("lim") ? constrain(server.arg("lim").toInt(), 0L, 3600L) : (long)S.limitS;
    int ans = server.hasArg("ans") ? server.arg("ans").toInt() : -1;
    startRound(lim > 0 ? lim * 1000 : 0, (ans >= 0 && ans <= 3) ? ans : 0xFF);
    ok("started");
  });

  onApi("/api/lock", MW_POST, []() { lockRound(); ok(); });

  onApi("/api/reveal", MW_POST, []() {
    int ans = server.hasArg("ans") ? server.arg("ans").toInt() : (correct == 0xFF ? -1 : correct);
    if (S.mode == 0 && (ans < 0 || ans > 3)) return fail("Choose the correct answer first");
    if (phase != PH_LOCKED && phase != PH_OPEN && phase != PH_COUNTDOWN) return fail("Nothing to reveal");
    // Lock now, reveal after a short grace so presses still in the air are counted.
    if (phase == PH_OPEN || phase == PH_COUNTDOWN) lockRound();
    revealAns = (ans >= 0 && ans <= 3) ? ans : 0xFF;
    revealPending = true;
    ok();
  });

  onApi("/api/reset", MW_POST, []() { resetRound(); ok(); });

  onApi("/api/player", MW_POST, []() {
    Player *p = argPlayer();
    if (!p) return fail("Unknown buzzer");
    if (server.hasArg("n")) {
      String n = server.arg("n"); n.trim();
      if (n.length()) strlcpy(p->d.name, n.c_str(), sizeof(p->d.name));
    }
    if (server.hasArg("c")) {
      String c = server.arg("c"); c.replace("#", "");
      p->d.color = strtoul(c.c_str(), nullptr, 16) & 0xFFFFFF;
    }
    savePlayers(); bump(); ok();
  });

  onApi("/api/identify", MW_POST, []() {
    Player *p = argPlayer();
    if (!p) return fail("Unknown buzzer");
    sendIdentify(p->d.mac); ok();
  });

  onApi("/api/remove", MW_POST, []() {
    Player *p = argPlayer();
    if (!p) return fail("Unknown buzzer");
    if (winner >= 0 && &players[winner] == p) winner = -1;
    p->used = false;
    savePlayers(); bump(); ok();
  });

  onApi("/api/score", MW_POST, []() {
    Player *p = argPlayer();
    if (!p) return fail("Unknown buzzer");
    if (server.hasArg("set")) p->d.score = server.arg("set").toInt();
    else p->d.score += server.arg("d").toInt();
    savePlayers(); bump(); ok();
  });

  onApi("/api/scores/reset", MW_POST, []() {
    for (auto &p : players) if (p.used) { p.d.score = 0; p.pts = 0; }
    savePlayers(); bump(); ok();
  });

  onApi("/api/settings", MW_POST, []() {
    if (server.hasArg("mode")) S.mode = constrain(server.arg("mode").toInt(), 0, 1);
    if (server.hasArg("cd")) S.countdownS = constrain(server.arg("cd").toInt(), 0, 30);
    if (server.hasArg("lim")) S.limitS = constrain(server.arg("lim").toInt(), 0, 600);
    if (server.hasArg("auto")) S.autoLock = server.arg("auto").toInt() != 0;
    if (server.hasArg("fs")) S.falseStart = server.arg("fs").toInt() != 0;
    if (server.hasArg("sq")) S.showQcd = server.arg("sq").toInt() != 0;
    if (server.hasArg("pw")) S.ptsWin = server.arg("pw").toInt();
    if (server.hasArg("pc")) S.ptsCorrect = server.arg("pc").toInt();
    if (server.hasArg("px")) S.ptsWrong = server.arg("px").toInt();
    if (server.hasArg("acc")) S.acceptNew = server.arg("acc").toInt() != 0;
    if (server.hasArg("title")) S.title = server.arg("title").substring(0, 60);
    saveSettings(); bump(); ok("saved");
  });

  onApi("/api/net", MW_POST, []() {
    String ap = server.arg("ap"); ap.trim();
    String pw = server.arg("pw");
    if (ap.length() < 1 || ap.length() > 31) return fail("Network name must be 1-31 characters");
    if (pw.length() && (pw.length() < 8 || pw.length() > 63)) return fail("Password must be 8-63 characters");
    S.apSsid = ap;
    if (pw.length()) S.apPass = pw;                       // empty = keep current password
    S.apCh = constrain(server.arg("ch").toInt(), 1, 13);
    String sta = server.arg("sta"); sta.trim();
    String stapw = server.arg("stapw");
    if (sta != S.staSsid || stapw.length()) S.staPass = stapw;  // empty = keep (same network)
    S.staSsid = sta;
    String h = server.arg("host"); h.trim();
    if (h.length()) S.hostName = h;
    saveSettings();
    ok("Saved. Host restarting - reconnect to the new Wi-Fi.");
    restartPending = true; restartAt = millis() + 800;
  });

  onApi("/api/restart", MW_POST, []() { ok("Restarting"); restartPending = true; restartAt = millis() + 500; });

  onApi("/api/factory", MW_POST, []() {
    for (auto &p : players) p.used = false;
    playersDirty = false;                  // don't write the old players back before restarting
    prefs.clear();
    LittleFS.remove("/questions.json");
    ok("Factory reset. Restarting");
    restartPending = true; restartAt = millis() + 500;
  });

  onApi("/api/btnreset", MW_POST, []() {
    uint8_t mac[6];
    bool one = strToMac(server.arg("m"), mac);
    for (auto &p : players) {
      if (!p.used || (one && memcmp(p.d.mac, mac, 6) != 0)) continue;
      for (int i = 0; i < 4; i++) p.btnBase[i] = p.btnPresses[i];
      p.glitchBase = p.btnGlitches;
    }
    bump(); ok();
  });

  // Log lines after ?after=<seq> (max 120 per call). Source "" = host.
  server.on("/api/logs", MW_GET, []() {
    uint32_t after = server.arg("after").toInt();
    if (after > logSeq) after = 0;                 // host restarted: send from the start
    String o;
    {
    GameLock g;                                  // build under the lock, send without it
    uint32_t first = logSeq > LOG_LINES ? logSeq - LOG_LINES + 1 : 1;
    if (after + 1 > first) first = after + 1;
    if (logSeq >= first && logSeq - first + 1 > 120) first = logSeq - 119;
    o.reserve(4096);
    o += "{\"last\":"; o += logSeq; o += ",\"now\":"; o += millis(); o += ",\"lines\":[";
    bool firstLine = true;
    for (uint32_t s = first; s <= logSeq && logSeq; s++) {
      const LogLine &L = logRing[(s - 1) % LOG_LINES];     // seq s lives at index s-1
      if (L.seq != s) continue;                              // cleared
      if (!firstLine) o += ',';
      firstLine = false;
      bool host = !(L.mac[0] | L.mac[1] | L.mac[2] | L.mac[3] | L.mac[4] | L.mac[5]);
      char mac[18] = "";
      if (!host) macToStr(L.mac, mac);
      o += "{\"s\":"; o += L.seq; o += ",\"t\":"; o += L.ms;
      o += ",\"m\":\""; o += mac; o += "\",\"x\":"; jsonStr(o, L.text); o += "}";
    }
    o += "]}";
    }
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", o);
  });

  onApi("/api/logs/clear", MW_POST, []() {
    for (auto &l : logRing) l.seq = 0;
    hlogf("[log] cleared");
    ok();
  });

  server.on("/api/questions", MW_GET, []() {
    File f = LittleFS.open("/questions.json", "r");
    if (!f) { server.send(200, "application/json", "[]"); return; }
    server.streamFile(f, "application/json");
    f.close();
  });

  server.on("/api/questions", MW_POST, []() {
    String body = server.arg("plain");
    if (!body.startsWith("[")) return fail("Expected a JSON list");
    File f = LittleFS.open("/questions.json", "w");
    if (!f) return fail("Storage error", 500);
    f.print(body);
    f.close();
    ok("saved");
  });

  server.onNotFound([]() {               // anything else -> app (handy for captive-style URLs)
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  });

  server.begin();
}

// ============================================================================
//  Setup / loop
// ============================================================================
void setup() {
  Serial.setTxBufferSize(2048);   // logging never blocks the game task
  Serial.begin(115200);
  delay(50);
  pinMode(LED_PIN, OUTPUT);
  gameMtx = xSemaphoreCreateRecursiveMutex();

  prefs.begin("kbchost", false);
  loadSettings();
  loadPlayers();
  roundId = (uint16_t)(esp_random() & 0x7FFF) + 1;

  if (!LittleFS.begin(true)) Serial.println("[fs] LittleFS mount failed");

  rxQ = xQueueCreate(48, sizeof(RxItem));

  // Wi-Fi: own access point (+ optional school Wi-Fi)
  bool useSta = S.staSsid.length() > 0;
  WiFi.setHostname(S.hostName.c_str());
  WiFi.mode(useSta ? WIFI_AP_STA : WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAP(S.apSsid.c_str(), S.apPass.length() ? S.apPass.c_str() : nullptr, S.apCh, 0, 8);
  if (useSta) {
    // Log school Wi-Fi drops: while the host reconnects, its radio scans other
    // channels and the buzzers can briefly lose it.
    WiFi.onEvent([](WiFiEvent_t e, WiFiEventInfo_t info) {
      if (e == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
        hlogf("[wifi] lost %s (reason %u), reconnecting...", S.staSsid.c_str(), info.wifi_sta_disconnected.reason);
      else if (e == ARDUINO_EVENT_WIFI_STA_GOT_IP)
        hlogf("[wifi] %s connected: %s (channel %u)", S.staSsid.c_str(), IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str(), WiFi.channel());
    });
    WiFi.setAutoReconnect(true);
    WiFi.begin(S.staSsid.c_str(), S.staPass.c_str());
    hlogf("[wifi] joining %s ...", S.staSsid.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(100);
  }
  esp_wifi_set_ps(WIFI_PS_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed, restarting");
    delay(1000);
    ESP.restart();
  }
  esp_now_register_recv_cb(onRecv);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST, 6);
  peer.channel = 0;
  peer.ifidx = WIFI_IF_AP;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  if (MDNS.begin(S.hostName.c_str())) MDNS.addService("http", "tcp", 80);
  setupWeb();
  // Both cores: game + radio on core 1 (high priority, alone), web server on
  // core 0 next to the Wi-Fi stack. A busy web page can't delay a press.
  xTaskCreatePinnedToCore(gameTask, "game", 8192, nullptr, 5, nullptr, 1);
  xTaskCreatePinnedToCore(webTask, "web", 12288, nullptr, 2, nullptr, 0);

  Serial.println();
  Serial.println("==============================================");
  Serial.println("  KBC Buzzer - HOST  v" FW_VERSION_STR);
  Serial.printf ("  Wi-Fi : %s   (password: %s)\n", S.apSsid.c_str(), S.apPass.length() ? S.apPass.c_str() : "<open>");
  Serial.printf ("  Open  : http://%s   or   http://%s.local\n", WiFi.softAPIP().toString().c_str(), S.hostName.c_str());
  if (useSta) Serial.printf("  School Wi-Fi: %s -> %s\n", S.staSsid.c_str(),
                            WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "not connected");
  Serial.printf ("  Radio channel: %u\n", WiFi.channel());
  Serial.println("==============================================");
  hlogf("[boot] host v" FW_VERSION_STR " ready on channel %u", WiFi.channel());
  if (useSta) hlogf("[wifi] %s: %s", S.staSsid.c_str(),
                    WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "not connected");
}

// Game + radio (core 1): wakes the moment a packet arrives, and at least every 2 ms.
static void gameTask(void *) {
  RxItem it;
  for (;;) {
    bool got = xQueueReceive(rxQ, &it, pdMS_TO_TICKS(2)) == pdTRUE;
    GameLock g;
    if (got) {
      handleRx(it);
      while (xQueueReceive(rxQ, &it, 0) == pdTRUE) handleRx(it);
    }
    tickRound();
    // State heartbeat: fast while a round is live, slower otherwise.
    uint32_t now = millis();
    uint32_t every = burst ? 25 : ((phase == PH_COUNTDOWN || phase == PH_OPEN) ? 150 : 400);
    if (now - lastStateTx >= every) {
      if (burst) burst--;
      sendState();
    }
  }
}

// Web server (core 0). Replies are queued and sent without waiting on the client.
static void webTask(void *) {
  for (;;) {
    server.handleClient();
    vTaskDelay(1);
  }
}

// Arduino loop (core 1, lowest priority): LED, deferred saves, restart.
void loop() {
  uint32_t now = millis();
  digitalWrite(LED_PIN, phase == PH_OPEN ? HIGH : (now % 2000 < 60));   // solid while answering
  if (playersDirty && (now - playersDirtyAt > 1500 || restartPending)) flushPlayers();
  if (restartPending && (int32_t)(now - restartAt) >= 0) { flushPlayers(); ESP.restart(); }
  delay(20);
}
