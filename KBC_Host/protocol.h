// ============================================================================
//  KBC Buzzer - shared radio protocol (ESP-NOW)
//  Keep this file IDENTICAL in KBC_Host and KBC_Node.
// ============================================================================
#pragma once
#include <stdint.h>

#define KBC_MAGIC 0xB7
#define KBC_PROTO 1

// Message types
enum : uint8_t {
  MSG_HELLO     = 1,  // node -> broadcast : "is there a host on this channel?"
  MSG_WELCOME   = 2,  // host -> broadcast : reply to HELLO (addressed by 'to')
  MSG_STATE     = 3,  // host -> broadcast : round state heartbeat
  MSG_PRESS     = 4,  // node -> host      : a button was pressed
  MSG_PRESS_ACK = 5,  // host -> broadcast : press received (addressed by 'to')
  MSG_IDENTIFY  = 6,  // host -> broadcast : blink so the teacher can find it
  MSG_PING      = 7,  // node -> host      : keep-alive
  MSG_BUTTONS   = 8,  // node -> host      : live button state (button test)
  MSG_LOG       = 9,  // node -> host      : one serial log line (web log viewer)
  MSG_BPRESS    = 10  // battery node -> host : press that woke the buzzer from sleep
};

// HelloMsg.buttons bit 7 set = battery buzzer (sleeps between presses)
#define HELLO_BATTERY_FLAG 0x80

// Round phases
enum : uint8_t {
  PH_IDLE      = 0,
  PH_COUNTDOWN = 1,
  PH_OPEN      = 2,
  PH_LOCKED    = 3,
  PH_REVEAL    = 4
};

// Press acknowledgement status
enum : uint8_t {
  ACK_ACCEPTED    = 1,
  ACK_DUPLICATE   = 2,  // already answered this round (first answer kept)
  ACK_CLOSED      = 3,  // too late, round is closed
  ACK_FALSE_START = 4,  // pressed during countdown -> locked out this round
  ACK_IGNORED     = 5   // pressed during countdown, no penalty -> try again
};

struct __attribute__((packed)) MsgHeader {
  uint8_t magic;
  uint8_t proto;
  uint8_t type;
};

struct __attribute__((packed)) HelloMsg {
  MsgHeader h;
  uint16_t fw;
  uint8_t  buttons;
};

struct __attribute__((packed)) WelcomeMsg {
  MsgHeader h;
  uint8_t  to[6];
  uint8_t  accepted;   // 0 = host is not accepting new buzzers
  uint8_t  playerNo;
  uint8_t  channel;
};

struct __attribute__((packed)) StateMsg {
  MsgHeader h;
  uint16_t roundId;
  uint8_t  phase;
  uint8_t  correct;     // 0..3, 0xFF = unknown (only filled in REVEAL)
  uint32_t elapsedUs;   // time since answering opened (OPEN / LOCKED / REVEAL)
  uint32_t countdownMs; // remaining countdown (COUNTDOWN)
  uint32_t limitMs;     // answer time limit, 0 = unlimited
  uint8_t  winner[6];   // MAC of the winner (REVEAL), zeros otherwise
};

struct __attribute__((packed)) PressMsg {
  MsgHeader h;
  uint16_t roundId;
  uint8_t  choice;      // 0=A 1=B 2=C 3=D
  uint8_t  early;       // 1 = pressed during countdown
  uint32_t reactionUs;  // measured on the node from the moment it saw "OPEN"
};

struct __attribute__((packed)) PressAckMsg {
  MsgHeader h;
  uint8_t  to[6];
  uint16_t roundId;
  uint8_t  status;
};

struct __attribute__((packed)) IdentifyMsg {
  MsgHeader h;
  uint8_t  to[6];
  uint16_t ms;
};

struct __attribute__((packed)) PingMsg {
  MsgHeader h;
  uint16_t roundId;
  uint8_t  choice;      // 0xFF = not answered
};

struct __attribute__((packed)) ButtonsMsg {
  MsgHeader h;
  uint8_t  held;        // bit0=A .. bit3=D, 1 = currently pressed
  uint16_t presses[4];  // clean presses counted since boot
  uint16_t glitches;    // contact bounces that did not last (loose wire sign)
};

// A battery buzzer sleeps during the round, so it cannot time the student from
// "GO". It reports how long ago (in µs) the button was pressed instead; the host
// subtracts that from the arrival time to get the real press moment.
struct __attribute__((packed)) BPressMsg {
  MsgHeader h;
  uint8_t  choice;      // 0=A 1=B 2=C 3=D
  uint8_t  flags;       // reserved
  uint16_t seq;         // same value on retries of one press
  uint32_t agoUs;       // time from button press until this packet was sent
};

#define LOG_TEXT_MAX 200
struct __attribute__((packed)) LogMsg {
  MsgHeader h;
  uint16_t seq;
  char     text[LOG_TEXT_MAX];  // not NUL-terminated; length = packet length - 5
};
