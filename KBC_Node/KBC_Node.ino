// ============================================================================
//  KBC Buzzer - STUDENT NODE firmware (ESP32)
//  ---------------------------------------------------------------------------
//  Flash this SAME sketch on every student buzzer. No per-device setup:
//  the buzzer finds the host by itself and the teacher names it in the web app.
//
//  Wiring (each button: one leg to the GPIO, the other leg to GND):
//      Button A -> GPIO 32      Button C -> GPIO 25
//      Button B -> GPIO 33      Button D -> GPIO 26
//      Status LED: on-board LED (GPIO 2)  - optional external LED + 220R
//      Buzzer (optional, ACTIVE buzzer): set BUZZER_PIN below, e.g. 27
//
//  LED meanings:
//      double blink ........ searching for host
//      tiny blink / 3 s .... connected, waiting
//      fast blink .......... ANSWER NOW!
//      solid ON ............ your answer is locked in
//      strobe .............. you are the WINNER
//      slow blink .......... you were correct (but not fastest)
//      3 quick flashes ..... false start (pressed too early)
//
//  Everything printed to Serial is also sent to the host, so it shows up in
//  the web app's "Logs" tab. Button state is sent live for the button test.
//
//  Board: "ESP32 Dev Module" (esp32 core 3.x)
// ============================================================================
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <Preferences.h>
#include <stdarg.h>
#include "protocol.h"

// ---------------------------- USER CONFIG -----------------------------------
static const uint8_t BTN_PINS[4] = {32, 33, 25, 26};  // A, B, C, D
#define LED_PIN        2      // status LED (-1 to disable)
#define LED_ON_LEVEL   HIGH   // HIGH for most dev boards
#define BUZZER_PIN     -1     // active buzzer pin, -1 = none
#define DEBOUNCE_MS    20     // button must stay pressed this long
#define HOST_TIMEOUT_MS 3500  // no news from host for this long -> search again
#define FW_VERSION     110    // 1.10
// ----------------------------------------------------------------------------

static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ---- radio receive queue (callback runs in the WiFi task) ------------------
struct RxItem {
  uint8_t mac[6];
  int64_t atUs;
  uint8_t len;
  uint8_t data[48];
};
static QueueHandle_t rxQ;

// ---- link state ------------------------------------------------------------
static Preferences prefs;
static uint8_t myMac[6];
static uint8_t hostMac[6];
static bool    hostKnown = false;   // remembered from last session
static bool    linked = false;
static uint8_t curChannel = 1;
static uint8_t playerNo = 0;
static uint32_t lastHostMs = 0;
static uint32_t lastHelloMs = 0;
static uint32_t lastPingMs = 0;

// ---- round state -----------------------------------------------------------
static uint16_t roundId = 0;
static uint8_t  phase = PH_IDLE;
static int64_t  openAtUs = 0;       // local estimate of when answering opened
static bool     pressed = false;
static uint8_t  myChoice = 0xFF;
static uint32_t myReactionUs = 0;
static bool     myEarly = false;
static bool     pressPending = false; // waiting for host ACK
static uint8_t  pressTries = 0;
static uint32_t lastPressSendMs = 0;
static uint8_t  ackStatus = 0;
static uint8_t  correctAns = 0xFF;
static bool     iAmWinner = false;
static uint32_t identifyUntil = 0;
static uint32_t flashUntil = 0;     // false-start flash

// ---- buttons ---------------------------------------------------------------
// Answer timing uses the interrupt edge (microsecond precise).
// The button test uses a debounced poll: held state, clean presses, glitches.
static volatile int64_t edgeUs[4] = {0, 0, 0, 0};
static bool     rawDown[4], stableDown[4], pendingLow[4];
static uint32_t rawChangeMs[4];
static uint16_t btnPresses[4];
static uint16_t btnGlitches = 0;
static bool     btnDirty = true;
static uint32_t lastBtnTxMs = 0;

void IRAM_ATTR onButtonEdge(void *arg) {
  int i = (int)(intptr_t)arg;
  if (edgeUs[i] == 0) edgeUs[i] = esp_timer_get_time();
}

// ---- log forwarding --------------------------------------------------------
#define LOG_Q 16
static char     logQ[LOG_Q][LOG_TEXT_MAX];
static uint8_t  logLen[LOG_Q];
static uint8_t  logHead = 0, logCount = 0;
static uint16_t logSeq = 0;
static uint32_t lastLogTxMs = 0;

// Print to Serial AND queue the line for the host's web log viewer.
static void nlog(const char *fmt, ...) {
  char line[LOG_TEXT_MAX + 1];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > LOG_TEXT_MAX) n = LOG_TEXT_MAX;
  Serial.println(line);
  uint8_t slot = (logHead + logCount) % LOG_Q;
  if (logCount == LOG_Q) { logHead = (logHead + 1) % LOG_Q; logCount--; }   // drop oldest
  memcpy(logQ[slot], line, n);
  logLen[slot] = (uint8_t)n;
  logCount++;
}

// ============================================================================
//  Helpers
// ============================================================================
static void macToStr(const uint8_t *m, char *out) {
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void fillHeader(MsgHeader &h, uint8_t type) {
  h.magic = KBC_MAGIC;
  h.proto = KBC_PROTO;
  h.type = type;
}

static void setChannel(uint8_t ch) {
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  curChannel = ch;
}

static void addPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0;            // follow current channel
  p.ifidx = WIFI_IF_STA;
  p.encrypt = false;
  esp_now_add_peer(&p);
}

static void buzz(uint16_t ms) {
#if BUZZER_PIN >= 0
  digitalWrite(BUZZER_PIN, HIGH);
  delay(ms);
  digitalWrite(BUZZER_PIN, LOW);
#else
  (void)ms;
#endif
}

// Delivery reports for messages sent straight to the host (radio-level ACK).
// If the host stops acknowledging us we are not really connected, even if we
// can still hear its broadcasts -> search again.
static volatile uint8_t sendFails = 0;
static volatile uint32_t sendOk = 0;
static void onSent(const uint8_t *mac, esp_now_send_status_t status) {
  if (mac[0] == 0xFF) return;                          // broadcasts have no ACK
  if (status == ESP_NOW_SEND_SUCCESS) { sendFails = 0; sendOk++; }
  else if (sendFails < 250) sendFails++;
}

static uint8_t radioChannel() {
  uint8_t ch = 0;
  wifi_second_chan_t sc;
  esp_wifi_get_channel(&ch, &sc);
  return ch;
}

static void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(MsgHeader) || len > (int)sizeof(RxItem::data)) return;
  if (data[0] != KBC_MAGIC || data[1] != KBC_PROTO) return;
  RxItem it;
  it.atUs = esp_timer_get_time();
  memcpy(it.mac, info->src_addr, 6);
  it.len = (uint8_t)len;
  memcpy(it.data, data, len);
  xQueueSend(rxQ, &it, 0);
}

// ============================================================================
//  Sending
// ============================================================================
static void sendHello() {
  HelloMsg m;
  fillHeader(m.h, MSG_HELLO);
  m.fw = FW_VERSION;
  m.buttons = 4;
  esp_now_send(BCAST, (uint8_t *)&m, sizeof(m));
}

static void sendPing() {
  PingMsg m;
  fillHeader(m.h, MSG_PING);
  m.roundId = roundId;
  m.choice = pressed ? myChoice : 0xFF;
  esp_now_send(hostMac, (uint8_t *)&m, sizeof(m));
}

static void sendPress() {
  PressMsg m;
  fillHeader(m.h, MSG_PRESS);
  m.roundId = roundId;
  m.choice = myChoice;
  m.early = myEarly ? 1 : 0;
  m.reactionUs = myReactionUs;
  esp_now_send(hostMac, (uint8_t *)&m, sizeof(m));
  lastPressSendMs = millis();
  pressTries++;
}

static void sendButtons() {
  ButtonsMsg m;
  fillHeader(m.h, MSG_BUTTONS);
  m.held = 0;
  for (int i = 0; i < 4; i++) { if (stableDown[i]) m.held |= 1 << i; m.presses[i] = btnPresses[i]; }
  m.glitches = btnGlitches;
  esp_now_send(hostMac, (uint8_t *)&m, sizeof(m));
  lastBtnTxMs = millis();
  btnDirty = false;
}

static void flushLogs() {
  if (!linked || logCount == 0 || millis() - lastLogTxMs < 8) return;
  LogMsg m;
  fillHeader(m.h, MSG_LOG);
  m.seq = ++logSeq;
  uint8_t n = logLen[logHead];
  memcpy(m.text, logQ[logHead], n);
  if (esp_now_send(hostMac, (uint8_t *)&m, sizeof(MsgHeader) + 2 + n) == ESP_OK) {
    logHead = (logHead + 1) % LOG_Q;
    logCount--;
  }
  lastLogTxMs = millis();
}

// ============================================================================
//  Receiving
// ============================================================================
static void newRound(uint16_t id) {
  roundId = id;
  openAtUs = 0;
  pressed = false;
  myChoice = 0xFF;
  myReactionUs = 0;
  myEarly = false;
  pressPending = false;
  pressTries = 0;
  ackStatus = 0;
  correctAns = 0xFF;
  iAmWinner = false;
  for (int i = 0; i < 4; i++) edgeUs[i] = 0;
}

static void becomeLinked(const uint8_t *mac) {
  memcpy(hostMac, mac, 6);
  addPeer(hostMac);
  bool was = linked;
  linked = true;
  lastHostMs = millis();
  btnDirty = true;                       // give the host our button state
  if (!was) {
    sendFails = 0;
    char s[18];
    macToStr(hostMac, s);
    nlog("[link] connected to host %s on channel %u (radio %u)", s, curChannel, radioChannel());
  }
  if (!hostKnown || prefs.getUChar("ch", 0) != curChannel) {
    prefs.putBytes("host", hostMac, 6);
    prefs.putUChar("ch", curChannel);
    hostKnown = true;
  }
}

static void handleState(const RxItem &it) {
  const StateMsg *m = (const StateMsg *)it.data;
  if (it.len < sizeof(StateMsg)) return;
  // While linked only listen to our own host.
  if (linked && memcmp(it.mac, hostMac, 6) != 0) return;
  // While searching: hearing the host is not enough, because Wi-Fi channels
  // overlap and a nearby host can be heard faintly on the wrong channel.
  // Ask for a WELCOME, which tells us the host's real channel.
  if (!linked) {
    if (millis() - lastHelloMs > 40) { lastHelloMs = millis(); sendHello(); }
    return;
  }
  lastHostMs = millis();

  if (m->roundId != roundId) newRound(m->roundId);
  uint8_t prev = phase;
  phase = m->phase;

  if (phase == PH_OPEN || phase == PH_LOCKED || phase == PH_REVEAL) {
    // Earliest estimate is the most accurate (radio delay only makes it later).
    int64_t est = it.atUs - (int64_t)m->elapsedUs;
    if (openAtUs == 0 || est < openAtUs) openAtUs = est;
  }
  if (phase == PH_REVEAL) {
    correctAns = m->correct;
    iAmWinner = memcmp(m->winner, myMac, 6) == 0;
    if (prev != PH_REVEAL) {
      if (iAmWinner) { nlog("[round %u] I WON", roundId); buzz(80); delay(60); buzz(80); delay(60); buzz(200); }
      else if (pressed && myChoice == correctAns) nlog("[round %u] correct, not fastest", roundId);
      else if (pressed) nlog("[round %u] wrong answer", roundId);
    }
  }
  if (prev != phase) {
    static const char *names[] = {"ready", "countdown", "answering", "locked", "revealed"};
    nlog("[round %u] %s", roundId, phase < 5 ? names[phase] : "?");
  }
}

static void handleRx(const RxItem &it) {
  const MsgHeader *h = (const MsgHeader *)it.data;
  switch (h->type) {
    case MSG_WELCOME: {
      if (it.len < sizeof(WelcomeMsg)) return;
      const WelcomeMsg *m = (const WelcomeMsg *)it.data;
      if (memcmp(m->to, myMac, 6) != 0) return;
      if (!m->accepted) {
        nlog("[link] host is not accepting new buzzers (turn on 'Accept new buzzers')");
        return;
      }
      if (linked && memcmp(it.mac, hostMac, 6) == 0) { lastHostMs = millis(); return; }  // extra replies to our HELLOs
      playerNo = m->playerNo;
      if (m->channel >= 1 && m->channel <= 13 && m->channel != radioChannel()) {
        nlog("[link] heard host on channel %u, moving to its real channel %u", radioChannel(), m->channel);
        setChannel(m->channel);
      }
      becomeLinked(it.mac);
      nlog("[link] I am player #%u", playerNo);
      break;
    }
    case MSG_STATE:
      handleState(it);
      break;
    case MSG_PRESS_ACK: {
      if (it.len < sizeof(PressAckMsg)) return;
      const PressAckMsg *m = (const PressAckMsg *)it.data;
      if (memcmp(m->to, myMac, 6) != 0 || m->roundId != roundId) return;
      if (!pressPending) return;                    // duplicate ack
      pressPending = false;
      ackStatus = m->status;
      static const char *st[] = {"?", "accepted", "duplicate", "too late", "FALSE START", "ignored - press again"};
      nlog("[press] host: %s", ackStatus < 6 ? st[ackStatus] : "?");
      if (ackStatus == ACK_IGNORED) {         // early press without penalty
        pressed = false;
        myChoice = 0xFF;
      } else if (ackStatus == ACK_FALSE_START) {
        flashUntil = millis() + 900;
      } else if (ackStatus == ACK_ACCEPTED) {
        buzz(40);
      }
      break;
    }
    case MSG_IDENTIFY: {
      if (it.len < sizeof(IdentifyMsg)) return;
      const IdentifyMsg *m = (const IdentifyMsg *)it.data;
      if (memcmp(m->to, myMac, 6) != 0) return;
      identifyUntil = millis() + m->ms;
      nlog("[identify] blinking");
      buzz(100);
      break;
    }
  }
}

// ============================================================================
//  Buttons
// ============================================================================
// Debounced poll for the button test (held state, press counts, glitches).
static void pollButtons() {
  uint32_t now = millis();
  for (int i = 0; i < 4; i++) {
    bool raw = digitalRead(BTN_PINS[i]) == LOW;
    if (raw != rawDown[i]) {
      rawDown[i] = raw;
      rawChangeMs[i] = now;
      if (raw && !stableDown[i]) pendingLow[i] = true;
    }
    if (now - rawChangeMs[i] < DEBOUNCE_MS) continue;
    if (raw != stableDown[i]) {                       // clean change
      stableDown[i] = raw;
      pendingLow[i] = false;
      if (raw) { btnPresses[i]++; nlog("[btn] %c pressed", 'A' + i); }
      btnDirty = true;
    } else if (pendingLow[i] && !raw) {               // went down but never settled
      pendingLow[i] = false;
      btnGlitches++;
      btnDirty = true;
      if (btnGlitches <= 5 || btnGlitches % 25 == 0) nlog("[btn] %c contact glitch (#%u) - check wiring", 'A' + i, btnGlitches);
    }
  }
  if (linked && ((btnDirty && now - lastBtnTxMs > 25) || now - lastBtnTxMs > 2000)) sendButtons();
}

// Answer handling: uses the precise interrupt timestamp of the first edge.
static void handleAnswerPress() {
  int64_t now = esp_timer_get_time();
  int best = -1;
  int64_t bestT = 0;
  for (int i = 0; i < 4; i++) {
    int64_t t = edgeUs[i];
    if (t == 0) continue;
    if (now - t < (int64_t)DEBOUNCE_MS * 1000) continue;    // wait to confirm
    bool stillDown = digitalRead(BTN_PINS[i]) == LOW;
    edgeUs[i] = 0;                                           // re-arm
    if (!stillDown) continue;                                // just noise / bounce
    if (best < 0 || t < bestT) { best = i; bestT = t; }
  }
  if (best < 0 || !linked || pressed) return;

  if (phase == PH_OPEN && openAtUs != 0) {
    int64_t r = bestT - openAtUs;
    if (r < 0) r = 0;
    myReactionUs = (uint32_t)r;
    myEarly = false;
  } else if (phase == PH_COUNTDOWN) {
    myReactionUs = 0;
    myEarly = true;
  } else {
    return;                                    // idle / locked / reveal
  }
  pressed = true;
  myChoice = (uint8_t)best;
  pressPending = true;
  pressTries = 0;
  sendPress();
  nlog("[press] answer %c  %.3f s%s", 'A' + best, myReactionUs / 1e6, myEarly ? " (EARLY)" : "");
}

// ============================================================================
//  LED
// ============================================================================
static void setLed(bool on) {
#if LED_PIN >= 0
  digitalWrite(LED_PIN, on ? LED_ON_LEVEL : !LED_ON_LEVEL);
#else
  (void)on;
#endif
}

static void updateLed() {
  uint32_t t = millis();
  if (t < identifyUntil) { setLed((t / 60) % 2); return; }
  if (t < flashUntil)    { setLed((t / 150) % 2); return; }
  if (!linked) {                                       // double blink
    uint32_t p = t % 1200;
    setLed(p < 80 || (p > 200 && p < 280));
    return;
  }
  switch (phase) {
    case PH_COUNTDOWN: setLed(false); return;
    case PH_OPEN:      setLed(pressed ? true : (t / 120) % 2); return;
    case PH_LOCKED:    setLed(pressed && ackStatus != ACK_FALSE_START); return;
    case PH_REVEAL:
      if (iAmWinner) { setLed((t / 50) % 2); return; }
      if (pressed && myChoice == correctAns) { setLed((t / 500) % 2); return; }
      setLed(false);
      return;
    default:                                           // idle heartbeat
      setLed(t % 3000 < 40);
  }
}

// ============================================================================
//  Setup / loop
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(50);

#if LED_PIN >= 0
  pinMode(LED_PIN, OUTPUT);
  setLed(false);
#endif
#if BUZZER_PIN >= 0
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
#endif
  for (int i = 0; i < 4; i++) {
    pinMode(BTN_PINS[i], INPUT_PULLUP);
    attachInterruptArg(digitalPinToInterrupt(BTN_PINS[i]), onButtonEdge, (void *)(intptr_t)i, FALLING);
  }
  delay(5);
  for (int i = 0; i < 4; i++) { rawDown[i] = stableDown[i] = digitalRead(BTN_PINS[i]) == LOW; rawChangeMs[i] = millis(); }

  rxQ = xQueueCreate(24, sizeof(RxItem));

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_get_mac(WIFI_IF_STA, myMac);

  prefs.begin("kbcnode", false);
  if (prefs.getBytes("host", hostMac, 6) == 6) hostKnown = true;
  curChannel = prefs.getUChar("ch", 1);
  if (curChannel < 1 || curChannel > 13) curChannel = 1;
  setChannel(curChannel);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed, restarting");
    delay(1000);
    ESP.restart();
  }
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);
  addPeer(BCAST);

  char s[18];
  macToStr(myMac, s);
  Serial.println();
  Serial.println("=====================================");
  Serial.println("  KBC Buzzer - student node");
  nlog("[boot] KBC buzzer fw %u.%02u  MAC %s", FW_VERSION / 100, FW_VERSION % 100, s);
  Serial.println("  Buttons A/B/C/D = GPIO 32/33/25/26");
  Serial.println("=====================================");
  for (int i = 0; i < 4; i++) if (stableDown[i]) nlog("[btn] WARNING: %c reads pressed at boot - stuck button or wiring short?", 'A' + i);
  nlog("[link] searching for host...");
}

void loop() {
  RxItem it;
  while (xQueueReceive(rxQ, &it, 0) == pdTRUE) handleRx(it);

  uint32_t now = millis();

  if (linked && now - lastHostMs > HOST_TIMEOUT_MS) {
    linked = false;
    nlog("[link] lost host, searching...");
  }
  if (linked && sendFails >= 6) {
    linked = false;
    sendFails = 0;
    nlog("[link] host is not receiving me (channel %u), searching...", radioChannel());
  }

  if (!linked) {
    // Hop through channels 1..13 saying HELLO until a host answers.
    if (now - lastHelloMs > 160) {
      lastHelloMs = now;
      static uint8_t stay = 0;
      if (++stay >= 2) {                     // 2 HELLOs per channel
        stay = 0;
        setChannel(curChannel % 13 + 1);
      }
      sendHello();
    }
  } else {
    if (now - lastPingMs > 1000) {
      lastPingMs = now;
      sendPing();
    }
    if (pressPending && now - lastPressSendMs > 40) {
      if (pressTries < 50) sendPress();
      else { pressPending = false; nlog("[press] no reply from host"); }
    }
  }

  pollButtons();
  handleAnswerPress();
  flushLogs();
  updateLed();
  delay(1);
}
