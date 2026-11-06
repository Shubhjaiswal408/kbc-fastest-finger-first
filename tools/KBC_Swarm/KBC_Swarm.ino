// ============================================================================
//  KBC Swarm - TEST TOOL, not for students.
//  One ESP32 pretends to be up to 20 buzzers, so a full class can be tested
//  with 2 boards. It sends the real ESP-NOW messages; each one ends with
//  [0x5A, simId] so the host (in test mode) treats it as a separate buzzer.
//
//  Battery sims behave like the C3: they "sleep", wake on a press, send a
//  BPRESS ~85-95 ms after the press, with how long ago the press was.
//  USB sims behave like KBC_Node: always listening, PING every second,
//  PRESS with the reaction time measured from when they heard "GO".
//
//  Driven over USB serial (921600) by tools/swarm_test.py:
//    C <count> <firstId>        set up sims
//    Y <id> b|u                 battery or USB sim
//    P <id> <delayMs> <choice>  press planned for the NEXT round
//                               (delay from GO; negative = during countdown)
//    P <id> x                   no press next round
//    Q <id> 0|1                 1 = switched off (silent)
//    H                          status
//  It prints:  L id playerNo | R round phase | F id round planMs sentUs | A id status
//
//  Board: "ESP32 Dev Module".
// ============================================================================
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include "protocol.h"

#define SIM_TAG 0x5A
#define MAXSIM  20

struct RxItem { uint8_t mac[6]; int64_t atUs; uint8_t len; uint8_t data[64]; };
struct Sim {
  uint8_t id = 0, vmac[6] = {0};
  bool battery = true, linked = false, silent = false;
  bool nextOn = false; int32_t nextDelay = 0; uint8_t nextChoice = 0;    // plan for next round
  bool on = false;     int32_t delayMs = 0;   uint8_t choice = 0;        // plan for this round
  bool fired = false, pending = false;
  int64_t pressAt = 0, sendAt = 0, lastSend = 0;
  uint32_t reactUs = 0; bool early = false; uint16_t seq = 0; int tries = 0;
  uint32_t lastHello = 0, lastPing = 0;
};

static Sim sims[MAXSIM];
static int nSim = 0;
static uint8_t myMac[6], hostMac[6];
static bool hostKnown = false;
static uint8_t channel = 1;
static uint16_t roundId = 0;
static uint8_t phase = 0;
static int64_t openAt = 0, cdEnd = 0;
static bool armed = false;
static QueueHandle_t rxQ;
static uint16_t seqGen = 1;

static void onRecv(const esp_now_recv_info_t *info, const uint8_t *d, int len) {
  if (len < 3 || len > 64 || d[0] != KBC_MAGIC) return;
  RxItem it;
  it.atUs = esp_timer_get_time();
  memcpy(it.mac, info->src_addr, 6);
  it.len = len;
  memcpy(it.data, d, len);
  xQueueSend(rxQ, &it, 0);
}

static void setChannel(uint8_t ch) { channel = ch; esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE); }

static void addPeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0; p.ifidx = WIFI_IF_STA; p.encrypt = false;
  esp_now_add_peer(&p);
}

// Send a message with the sim tag appended. Retries briefly if the radio queue is full.
static bool tx(const uint8_t *to, const void *msg, size_t len, uint8_t id) {
  uint8_t b[64];
  memcpy(b, msg, len);
  b[len] = SIM_TAG; b[len + 1] = id;
  for (int i = 0; i < 20; i++) {
    esp_err_t e = esp_now_send(to, b, len + 2);
    if (e == ESP_OK) return true;
    delayMicroseconds(500);
  }
  return false;
}
static void hdr(MsgHeader &h, uint8_t t) { h.magic = KBC_MAGIC; h.proto = KBC_PROTO; h.type = t; }

static Sim *simByMac(const uint8_t *m) {
  for (int i = 0; i < nSim; i++) if (memcmp(sims[i].vmac, m, 6) == 0) return &sims[i];
  return nullptr;
}
static Sim *simById(int id) {
  for (int i = 0; i < nSim; i++) if (sims[i].id == id) return &sims[i];
  return nullptr;
}

static void sendHello(Sim &s) {
  static const uint8_t BC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  HelloMsg m; hdr(m.h, MSG_HELLO); m.fw = 900; m.buttons = 4 | (s.battery ? HELLO_BATTERY_FLAG : 0);
  tx(hostKnown ? hostMac : BC, &m, sizeof(m), s.id);
  s.lastHello = millis();
}

static void sendPress(Sim &s) {
  if (s.battery) {
    BPressMsg m; hdr(m.h, MSG_BPRESS);
    m.choice = s.choice; m.flags = 0; m.seq = s.seq;
    m.agoUs = (uint32_t)(esp_timer_get_time() - s.pressAt);
    tx(hostMac, &m, sizeof(m), s.id);
  } else {
    PressMsg m; hdr(m.h, MSG_PRESS);
    m.roundId = roundId; m.choice = s.choice; m.early = s.early; m.reactionUs = s.reactUs;
    tx(hostMac, &m, sizeof(m), s.id);
  }
  s.lastSend = esp_timer_get_time();
  s.tries++;
}

static void newRound(uint16_t id) {
  roundId = id; openAt = 0; cdEnd = 0;
  for (int i = 0; i < nSim; i++) {
    Sim &s = sims[i];
    s.fired = s.pending = false; s.tries = 0;
    s.on = armed && s.nextOn; s.delayMs = s.nextDelay; s.choice = s.nextChoice;
    s.nextOn = false;
  }
  armed = false;
}

static void handleRx(const RxItem &it) {
  switch (it.data[2]) {
    case MSG_WELCOME: {
      if (it.len < sizeof(WelcomeMsg)) return;
      const WelcomeMsg *m = (const WelcomeMsg *)it.data;
      Sim *s = simByMac(m->to);
      if (!s || !m->accepted) return;
      if (!hostKnown) {
        memcpy(hostMac, it.mac, 6); addPeer(hostMac); hostKnown = true;
        if (m->channel >= 1 && m->channel <= 13) setChannel(m->channel);
        Serial.printf("I host %02X:%02X:%02X:%02X:%02X:%02X channel %u\n", it.mac[0], it.mac[1], it.mac[2], it.mac[3], it.mac[4], it.mac[5], channel);
      }
      if (!s->linked) { s->linked = true; Serial.printf("L %u %u\n", s->id, m->playerNo); }
      break;
    }
    case MSG_STATE: {
      if (!hostKnown || memcmp(it.mac, hostMac, 6) != 0 || it.len < sizeof(StateMsg)) return;
      const StateMsg *m = (const StateMsg *)it.data;
      if (m->roundId != roundId) newRound(m->roundId);
      if (m->phase != phase) Serial.printf("R %u %u\n", roundId, m->phase);
      phase = m->phase;
      if (phase == PH_COUNTDOWN) {
        int64_t e = it.atUs + (int64_t)m->countdownMs * 1000;
        if (cdEnd == 0 || e < cdEnd) cdEnd = e;
      }
      if (phase >= PH_OPEN) {
        int64_t e = it.atUs - (int64_t)m->elapsedUs;
        if (openAt == 0 || e < openAt) openAt = e;
      }
      break;
    }
    case MSG_PRESS_ACK: {
      if (it.len < sizeof(PressAckMsg)) return;
      const PressAckMsg *m = (const PressAckMsg *)it.data;
      Sim *s = simByMac(m->to);
      if (!s || !s->pending) return;
      if (!s->battery && m->roundId != roundId) return;
      s->pending = false;
      Serial.printf("A %u %u %d\n", s->id, m->status, s->tries);
      break;
    }
  }
}

static void firePresses() {
  int64_t now = esp_timer_get_time();
  for (int i = 0; i < nSim; i++) {
    Sim &s = sims[i];
    if (!s.on || s.fired || s.silent || !s.linked) continue;
    int64_t T;
    if (s.delayMs >= 0) { if (openAt == 0) continue; T = openAt + (int64_t)s.delayMs * 1000; }
    else                { if (cdEnd == 0) continue;  T = cdEnd + (int64_t)s.delayMs * 1000; }
    if (s.battery) {
      // asleep: the press wakes it, it sends ~85-95 ms later, whatever the phase
      int64_t wake = (85 + s.id % 11) * 1000;
      if (now < T + wake) continue;
      s.pressAt = T; s.seq = seqGen++;
    } else {
      if (now < T) continue;
      if (phase == PH_OPEN && openAt) { s.reactUs = (uint32_t)(now - openAt); s.early = false; }
      else if (phase == PH_COUNTDOWN) { s.reactUs = 0; s.early = true; }
      else { s.fired = true; Serial.printf("F %u %u %ld -1\n", s.id, roundId, (long)s.delayMs); continue; }  // a real node ignores it
      s.pressAt = now;
    }
    s.fired = true; s.pending = true; s.tries = 0;
    sendPress(s);
    Serial.printf("F %u %u %ld %ld\n", s.id, roundId, (long)s.delayMs, (long)(s.pressAt - (openAt ? openAt : cdEnd)));
  }
}

static void handleSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') { line += c; continue; }
    line.trim();
    if (!line.length()) continue;
    char cmd = line[0];
    int a = 0; long b = 0; char t[8] = {0};
    if (cmd == 'C' && sscanf(line.c_str() + 1, "%d %ld", &a, &b) == 2) {
      nSim = constrain(a, 0, MAXSIM);
      for (int i = 0; i < nSim; i++) {
        sims[i] = Sim();
        sims[i].id = (uint8_t)(b + i);
        uint8_t v[6] = {0x02, myMac[3], myMac[4], myMac[5], SIM_TAG, sims[i].id};
        memcpy(sims[i].vmac, v, 6);
        sims[i].lastHello = millis() + i * 37;
      }
      Serial.printf("OK C %d\n", nSim);
    } else if (cmd == 'Y' && sscanf(line.c_str() + 1, "%d %7s", &a, t) == 2) {
      Sim *s = simById(a); if (s) { s->battery = t[0] == 'b'; s->linked = false; }
      Serial.println("OK Y");
    } else if (cmd == 'P') {
      long d; int ch;
      if (sscanf(line.c_str() + 1, "%d %ld %d", &a, &d, &ch) == 3) {
        Sim *s = simById(a); if (s) { s->nextOn = true; s->nextDelay = d; s->nextChoice = ch & 3; armed = true; }
      } else if (sscanf(line.c_str() + 1, "%d", &a) == 1) {
        Sim *s = simById(a); if (s) s->nextOn = false; armed = true;
      }
    } else if (cmd == 'Q' && sscanf(line.c_str() + 1, "%d %ld", &a, &b) == 2) {
      Sim *s = simById(a); if (s) s->silent = b != 0;
      Serial.println("OK Q");
    } else if (cmd == 'H') {
      int l = 0; for (int i = 0; i < nSim; i++) l += sims[i].linked;
      Serial.printf("S sims %d linked %d host %d ch %u round %u phase %u\n", nSim, l, hostKnown, channel, roundId, phase);
    }
    line = "";
  }
}

void setup() {
  Serial.setTxBufferSize(4096);
  Serial.begin(921600);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  rxQ = xQueueCreate(64, sizeof(RxItem));
  esp_now_init();
  esp_now_register_recv_cb(onRecv);
  static const uint8_t BC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  addPeer(BC);
  setChannel(1);
  Serial.printf("\nI swarm ready %02X:%02X:%02X:%02X:%02X:%02X\n", myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5]);
}

void loop() {
  RxItem it;
  while (xQueueReceive(rxQ, &it, 0) == pdTRUE) handleRx(it);
  handleSerial();
  uint32_t now = millis();

  if (!hostKnown && nSim > 0) {               // hop channels until the host answers
    static uint32_t last = 0; static uint8_t stay = 0;
    if (now - last > 120) {
      last = now;
      if (++stay >= 2) { stay = 0; setChannel(channel % 13 + 1); }
      sendHello(sims[0]);
    }
  }
  for (int i = 0; i < nSim && hostKnown; i++) {
    Sim &s = sims[i];
    if (s.silent) continue;
    if (!s.linked && (int32_t)(now - s.lastHello) > 300) sendHello(s);
    if (s.linked && !s.battery && now - s.lastPing > 1000 + s.id * 7) {
      PingMsg m; hdr(m.h, MSG_PING); m.roundId = roundId; m.choice = 0xFF;
      tx(hostMac, &m, sizeof(m), s.id);
      s.lastPing = now;
    }
  }
  firePresses();
  int64_t t = esp_timer_get_time();
  for (int i = 0; i < nSim; i++) {            // resend until the host answers (like the real buzzers)
    Sim &s = sims[i];
    if (s.pending && t - s.lastSend > 50000) {
      if (s.tries < 20) sendPress(s);
      else { s.pending = false; Serial.printf("A %u 0 %d\n", s.id, s.tries); }
    }
  }
  delay(1);
}
