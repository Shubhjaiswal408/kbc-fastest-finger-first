// ============================================================================
//  KBC Buzzer - LOW-POWER BATTERY BUZZER for Seeed XIAO ESP32-C6
//  ---------------------------------------------------------------------------
//  Sleeps (about 15 µA) until a button is pressed, sends the answer to the host,
//  flashes the LED with the result and goes back to sleep. Awake about 0.5 s per
//  press. Target: 1-2+ years on 2 x AA.
//
//  Wiring (each button: one leg to the pin, the other leg to GND):
//      Button A -> D0  (GPIO 0)
//      Button B -> D1  (GPIO 1)
//      Button C -> D2  (GPIO 2)
//      Button D -> MTDO pad on the back (GPIO 7)
//      (Only GPIO 0-7 can wake the C6 from deep sleep; D3-D10 cannot.)
//      LED: built-in user LED (GPIO 15)
//      Power: 2 x AA (+) -> 3V3 pin, (-) -> GND. Best: through a low-drain
//             buck-boost converter set to 3.3 V (e.g. TPS63900) so the full
//             battery is used. Do NOT connect USB and AA at the same time.
//
//  LED after a press:
//      one long flash (0.6 s) ...... answer accepted
//      two short flashes ........... already answered this round
//      three fast flashes .......... false start (pressed during countdown)
//      double blink ................ not an option for this question - press another
//      one tiny blink .............. no round running (answers closed)
//      five fast flashes ........... host not found
//  After power-on: blinks while searching, two long flashes when connected.
//
//  Updating this firmware: the board is asleep most of the time, so USB is
//  gone. Press RESET (or re-plug USB): it stays awake ~8 s, enough to upload.
//  Or hold BOOT while plugging in.
//
//  Board: "XIAO_ESP32C6" (esp32 core 3.x), USB CDC On Boot: Enabled
// ============================================================================
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <driver/gpio.h>
#include <Preferences.h>
#include "protocol.h"

extern "C" uint64_t esp_rtc_get_time_us(void);   // RTC clock, keeps running in deep sleep

// ---------------------------- USER CONFIG -----------------------------------
static const gpio_num_t BTN[4] = {GPIO_NUM_0, GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_7};  // A B C D
#define LED_PIN          15          // XIAO C6 user LED
#define LED_ON_LEVEL     LOW         // the XIAO C6 LED lights when the pin is LOW
#define HEARTBEAT_MIN    60          // check in with the host every N minutes (0 = never)
#define POWERON_AWAKE_MS 8000        // stay awake after power-on/reset (time to upload firmware)
#define FW_VERSION       200         // 2.00 (battery)
// ----------------------------------------------------------------------------

#define STATE_MAGIC 0x4B424331UL
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Kept in low-power RAM across deep sleep.
struct State {
  uint32_t magic;
  uint8_t  host[6];
  uint8_t  hostKnown;
  uint8_t  ch;
  uint32_t wakeLatUs;      // measured: wake-up -> setup(), added to every press time
  uint64_t calibWakeAt;    // RTC time the calibration timer was due
  uint8_t  calibrating;
  uint16_t presses[4];
  uint16_t glitches;
  uint16_t seq;
  uint32_t wakes;
};
RTC_DATA_ATTR static State R;

static Preferences prefs;
static uint8_t  myMac[6];
static int64_t  usAtSetup = 0;      // esp_timer value at the start of setup()

// radio replies (filled by the receive callback)
static volatile bool    gotWelcome = false;
static volatile uint8_t welcomeCh = 0;
static uint8_t          welcomeMac[6];
static volatile int     ackStatus = -1;
static volatile int64_t ackAtUs = 0;

// ============================================================================
//  Small helpers
// ============================================================================
static void led(bool on) { digitalWrite(LED_PIN, on ? LED_ON_LEVEL : !LED_ON_LEVEL); }
static void blink(int n, int onMs, int offMs) {
  for (int i = 0; i < n; i++) { led(true); delay(onMs); led(false); if (i < n - 1) delay(offMs); }
}
static void fillHeader(MsgHeader &h, uint8_t type) { h.magic = KBC_MAGIC; h.proto = KBC_PROTO; h.type = type; }

// Microseconds since the button was pressed (= since the chip woke up).
static uint32_t agoUs() { return R.wakeLatUs + (uint32_t)(esp_timer_get_time() - usAtSetup); }

static void setChannel(uint8_t ch) {
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
}

static void addPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0;
  p.ifidx = WIFI_IF_STA;
  p.encrypt = false;
  esp_now_add_peer(&p);
}

static void onRecv(const esp_now_recv_info_t *info, const uint8_t *d, int len) {
  if (len < (int)sizeof(MsgHeader) || d[0] != KBC_MAGIC || d[1] != KBC_PROTO) return;
  if (d[2] == MSG_WELCOME && len >= (int)sizeof(WelcomeMsg)) {
    const WelcomeMsg *m = (const WelcomeMsg *)d;
    if (memcmp(m->to, myMac, 6) || !m->accepted) return;
    memcpy(welcomeMac, info->src_addr, 6);
    welcomeCh = m->channel;
    gotWelcome = true;
  } else if (d[2] == MSG_PRESS_ACK && len >= (int)sizeof(PressAckMsg)) {
    const PressAckMsg *m = (const PressAckMsg *)d;
    if (memcmp(m->to, myMac, 6)) return;
    if (ackStatus < 0) { ackStatus = m->status; ackAtUs = esp_timer_get_time(); }
  }
}

// ============================================================================
//  Radio
// ============================================================================
static void radioUp() {
  WiFi.mode(WIFI_STA);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  setChannel(R.ch >= 1 && R.ch <= 13 ? R.ch : 1);
  esp_now_init();
  esp_now_register_recv_cb(onRecv);
  addPeer(BCAST);
  if (R.hostKnown) addPeer(R.host);
}

static void radioDown() {
  delay(8);                          // let the last packet leave
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
}

static void sendHello() {
  HelloMsg m;
  fillHeader(m.h, MSG_HELLO);
  m.fw = FW_VERSION;
  m.buttons = 4 | HELLO_BATTERY_FLAG;
  esp_now_send(BCAST, (uint8_t *)&m, sizeof(m));
}

// Ask "is my host on this channel?" and wait for the reply.
static bool helloOn(uint8_t ch, uint32_t waitMs) {
  setChannel(ch);
  gotWelcome = false;
  sendHello();
  uint32_t t0 = millis();
  while (!gotWelcome && millis() - t0 < waitMs) delay(2);
  if (!gotWelcome) return false;
  // The host tells us its real channel (we may have heard it on a neighbouring one).
  uint8_t real = (welcomeCh >= 1 && welcomeCh <= 13) ? welcomeCh : ch;
  setChannel(real);
  bool changed = !R.hostKnown || memcmp(R.host, welcomeMac, 6) || R.ch != real;
  memcpy(R.host, welcomeMac, 6);
  R.hostKnown = 1;
  R.ch = real;
  addPeer(R.host);
  if (changed) { prefs.putBytes("host", R.host, 6); prefs.putUChar("ch", R.ch); }
  return true;
}

// Try the remembered channel first, then all channels, until timeout.
static bool findHost(uint32_t budgetMs, bool blinkLed) {
  uint32_t t0 = millis();
  if (R.ch >= 1 && R.ch <= 13 && helloOn(R.ch, 120)) return true;
  while (millis() - t0 < budgetMs) {
    for (uint8_t ch = 1; ch <= 13 && millis() - t0 < budgetMs; ch++) {
      if (blinkLed) led((millis() / 150) % 2);
      if (helloOn(ch, 60)) { led(false); return true; }
    }
  }
  led(false);
  return false;
}

static void sendButtons() {
  if (!R.hostKnown) return;
  ButtonsMsg m;
  fillHeader(m.h, MSG_BUTTONS);
  m.held = 0;
  for (int i = 0; i < 4; i++) m.presses[i] = R.presses[i];
  m.glitches = R.glitches;
  esp_now_send(R.host, (uint8_t *)&m, sizeof(m));
}

static void sendLog(const char *fmt, ...) {
  char line[LOG_TEXT_MAX + 1];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > LOG_TEXT_MAX) n = LOG_TEXT_MAX;
  Serial.println(line);
  if (!R.hostKnown) return;
  LogMsg m;
  fillHeader(m.h, MSG_LOG);
  m.seq = R.seq;
  memcpy(m.text, line, n);
  esp_now_send(R.host, (uint8_t *)&m, sizeof(MsgHeader) + 2 + n);
  delay(4);
}

// ============================================================================
//  Sleep
// ============================================================================
static void goSleep(uint64_t timerUs) {
  led(false);
  // Wait (max 3 s) for the buttons to be released; a held button would wake us at once.
  uint32_t t0 = millis();
  uint64_t mask = 0;
  for (;;) {
    mask = 0;
    int up = 0;
    for (int i = 0; i < 4; i++) if (digitalRead(BTN[i]) == HIGH) { mask |= 1ULL << BTN[i]; up++; }
    if (up == 4 || millis() - t0 > 3000) break;
    delay(10);
  }
  for (int i = 0; i < 4; i++) {
    if (!(mask & (1ULL << BTN[i]))) { R.glitches++; continue; }   // stuck/held: leave it out this time
    rtc_gpio_init(BTN[i]);
    rtc_gpio_set_direction(BTN[i], RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis(BTN[i]);
    rtc_gpio_pullup_en(BTN[i]);
  }
  if (mask) esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);
#if SOC_PM_SUPPORT_RTC_PERIPH_PD
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);   // keep pull-ups alive
#endif
  if (timerUs) esp_sleep_enable_timer_wakeup(timerUs);

  // Antenna switch off while asleep (held until the next wake-up).
  digitalWrite(WIFI_ENABLE, HIGH);
  gpio_hold_en((gpio_num_t)WIFI_ENABLE);

  Serial.flush();
  if (R.calibrating) R.calibWakeAt = esp_rtc_get_time_us() + timerUs;   // as late as possible
  esp_deep_sleep_start();
}

static uint64_t heartbeatUs() { return HEARTBEAT_MIN ? (uint64_t)HEARTBEAT_MIN * 60ULL * 1000000ULL : 0; }

// ============================================================================
//  Wake reasons
// ============================================================================
static void onButtonWake(uint64_t ext1) {
  int choice = -1;
  for (int i = 0; i < 4; i++) if (ext1 & (1ULL << BTN[i])) { choice = i; break; }
  if (choice < 0) { R.glitches++; goSleep(heartbeatUs()); }

  R.presses[choice]++;
  uint16_t seq = ++R.seq;
  radioUp();

  BPressMsg m;
  fillHeader(m.h, MSG_BPRESS);
  m.choice = choice;
  m.flags = 0;
  m.seq = seq;

  // Send (and resend every 40 ms) until the host answers.
  ackStatus = -1;
  int64_t firstSendUs = 0;
  uint32_t firstAgo = 0;
  bool searched = false;
  uint32_t t0 = millis();
  while (ackStatus < 0) {
    m.agoUs = agoUs();
    if (!firstSendUs) { firstSendUs = esp_timer_get_time(); firstAgo = m.agoUs; }
    esp_now_send(R.hostKnown ? R.host : BCAST, (uint8_t *)&m, sizeof(m));
    uint32_t w = millis();
    while (ackStatus < 0 && millis() - w < 40) delay(1);
    if (ackStatus >= 0) break;
    if (millis() - t0 > 300 && !searched) {      // not on the remembered channel -> search
      searched = true;
      if (!findHost(2500, false)) break;
      t0 = millis();
    } else if (searched && millis() - t0 > 400) break;
  }

  int st = ackStatus;
  static const char *names[] = {"no reply", "accepted", "already answered", "answers closed", "FALSE START", "ignored (not an option)"};
  sendButtons();
  char ackTxt[24] = "";
  if (st >= 0 && firstSendUs) snprintf(ackTxt, sizeof(ackTxt), ", reply %ld ms", (long)((ackAtUs - firstSendUs) / 1000));
  sendLog("[press] %c -> %s | press->send %lu ms%s | wake #%lu", 'A' + choice,
          (st >= 1 && st <= 5) ? names[st] : names[0], (unsigned long)(firstAgo / 1000), ackTxt,
          (unsigned long)R.wakes);
  radioDown();

  // LED feedback with the radio off (cheap).
  switch (st) {
    case ACK_ACCEPTED:    led(true); delay(600); break;
    case ACK_DUPLICATE:   blink(2, 120, 120); break;
    case ACK_FALSE_START: blink(3, 70, 70); break;
    case ACK_IGNORED:     blink(2, 40, 90); break;
    case ACK_CLOSED:      blink(1, 40, 0); break;
    default:              blink(5, 50, 50); break;   // host not found
  }
  goSleep(heartbeatUs());
}

static void onHeartbeat() {
  radioUp();
  bool ok = findHost(1500, false);
  if (ok) { sendButtons(); sendLog("[heartbeat] sleeping, %lu wakes so far", (unsigned long)R.wakes); }
  radioDown();
  goSleep(heartbeatUs());
}

static void onPowerOn() {
  Serial.println();
  Serial.println("=====================================");
  Serial.println("  KBC Buzzer - battery (XIAO ESP32-C6)");
  Serial.println("  Buttons A/B/C/D = D0/D1/D2/MTDO(GPIO7)");
  Serial.println("=====================================");
  radioUp();
  char s[18];
  sprintf(s, "%02X:%02X:%02X:%02X:%02X:%02X", myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5]);
  Serial.printf("MAC %s  fw %u.%02u\n", s, FW_VERSION / 100, FW_VERSION % 100);
  bool ok = findHost(POWERON_AWAKE_MS - 1000, true);
  if (ok) {
    sendButtons();
    sendLog("[boot] battery buzzer fw %u.%02u  MAC %s  channel %u  wake delay %lu us", FW_VERSION / 100, FW_VERSION % 100,
            s, R.ch, (unsigned long)R.wakeLatUs);
    radioDown();
    blink(2, 400, 200);
  } else {
    Serial.println("[link] host not found - will try again on the next press");
    radioDown();
    blink(5, 50, 50);
  }
  // Stay awake a little so new firmware can be uploaded right after a reset.
  while (millis() < POWERON_AWAKE_MS) { led((millis() % 1000) < 30); delay(20); }

  // Measure this board's wake-up delay once: sleep 0.3 s on a timer and see how
  // late setup() starts. A button wake-up takes the same path.
  R.calibrating = 1;
  goSleep(300000ULL);
}

static void onCalibrationWake(uint64_t rtcAtSetup) {
  R.calibrating = 0;
  int64_t lat = (int64_t)rtcAtSetup - (int64_t)R.calibWakeAt;
  if (lat > 1000 && lat < 2000000) {
    R.wakeLatUs = (uint32_t)lat;
    prefs.putULong("wlat", R.wakeLatUs);
  }
  Serial.printf("[calibrate] wake-up delay %lu us\n", (unsigned long)R.wakeLatUs);
  goSleep(heartbeatUs());
}

// ============================================================================
void setup() {
  uint64_t rtcAtSetup = esp_rtc_get_time_us();
  usAtSetup = esp_timer_get_time();
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  uint64_t ext1 = cause == ESP_SLEEP_WAKEUP_EXT1 ? esp_sleep_get_ext1_wakeup_status() : 0;

  // Antenna switch on again (it was held off during sleep).
  gpio_hold_dis((gpio_num_t)WIFI_ENABLE);
  pinMode(WIFI_ENABLE, OUTPUT);
  digitalWrite(WIFI_ENABLE, LOW);
  pinMode(WIFI_ANT_CONFIG, OUTPUT);
  digitalWrite(WIFI_ANT_CONFIG, LOW);          // built-in antenna

  pinMode(LED_PIN, OUTPUT);
  led(false);
  for (int i = 0; i < 4; i++) { rtc_gpio_deinit(BTN[i]); pinMode(BTN[i], INPUT_PULLUP); }

  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);                    // never wait for USB when running on batteries

  prefs.begin("kbcbat", false);
  if (R.magic != STATE_MAGIC) {                // first start after power was connected
    memset(&R, 0, sizeof(R));
    R.magic = STATE_MAGIC;
    R.hostKnown = prefs.getBytes("host", R.host, 6) == 6;
    R.ch = prefs.getUChar("ch", 1);
    R.wakeLatUs = prefs.getULong("wlat", 60000);   // replaced by the measurement below
  }
  R.wakes++;

  if (cause == ESP_SLEEP_WAKEUP_TIMER && R.calibrating) onCalibrationWake(rtcAtSetup);
  else if (cause == ESP_SLEEP_WAKEUP_EXT1) onButtonWake(ext1);
  else if (cause == ESP_SLEEP_WAKEUP_TIMER) onHeartbeat();
  else onPowerOn();
}

void loop() {
  goSleep(heartbeatUs());   // not reached
}
