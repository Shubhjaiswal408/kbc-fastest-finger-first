# KBC Buzzer: Fastest Finger First for the classroom

A wireless quiz-buzzer system built on ESP32, inspired by the "Fastest Finger First" round of
*Kaun Banega Crorepati*. The teacher runs the quiz from a laptop; every student has a small
buzzer with four buttons (**A B C D**). The **fastest correct answer wins**, timed to the millisecond.

![Big screen: results](docs/images/stage-results.png)

## Features

- **No router needed.** Buzzers talk to the host over ESP-NOW (direct ESP-to-ESP radio, ~1-3 ms).
- **Fair timing.** Each buzzer times the student from the moment it heard "GO", so radio delay never decides the winner.
- **KBC-style big screen** for the projector: countdown, question, 2-4 options, timer ring, ranked results, confetti.
- **Teacher console** in any browser, with keyboard shortcuts (Space = next step).
- **Zero setup per buzzer.** Same firmware on every buzzer; it pairs itself, and you name it in the app.
- **Question bank** with 2, 3 or 4 options per question, paste-many, JSON import/export.
- **Scoreboard and round history** (CSV export).
- **Built-in tools:** live button test for every buzzer and a Logs tab that shows every device's serial output, no USB cable needed.
- **Optional battery buzzer** on a Seeed XIAO ESP32-C6 that sleeps between presses (target: 1-2 years on 2 x AA). *Beta, see below.*
- Everything is configurable from the web app: rules, points, timing, Wi-Fi, titles, sounds.

| Teacher console | Big screen (question) |
|---|---|
| ![Play tab](docs/images/play.png) | ![Big screen question](docs/images/stage-question.png) |

## How it works

![System overview](docs/images/system.svg)

| Part | Board | Firmware |
|---|---|---|
| Host (one) | Any ESP32 dev board | [`KBC_Host`](KBC_Host) |
| Buzzer, USB powered | Any ESP32 dev board + 4 buttons | [`KBC_Node`](KBC_Node) |
| Buzzer, battery (beta) | Seeed XIAO ESP32-C6 + 4 buttons + 2 x AA | [`KBC_BatteryC6`](KBC_BatteryC6) |
| Buzzer, battery (beta) | ESP32-C3 SuperMini + 4 buttons + 2-3 x AA | [`KBC_BatteryC3`](KBC_BatteryC3) |

## Quick start

1. **Wire the buzzers:** 4 buttons from GPIO 32/33/25/26 to GND. See the [hardware guide](docs/HARDWARE.md).
2. **Flash the firmware** with the Arduino IDE (esp32 core 3.x, no extra libraries):
   - one board with `KBC_Host` (*ESP32 Dev Module*)
   - every buzzer with `KBC_Node` (*ESP32 Dev Module*)
3. **Power the host** and connect your laptop to the Wi-Fi **KBC-Quiz** (password `kbc12345`).
   Open **http://192.168.4.1** (or **http://kbc.local**).
4. **Switch on the buzzers.** They appear on the *Buzzers* tab by themselves.
5. Press **Big screen**, then **Space** to start a round.

Full walkthrough with screenshots: **[User guide](docs/USER_GUIDE.md)**.

Want to look around without hardware? Open `web/index.html` in a browser (or add `?demo` to the URL):
it runs with simulated buzzers.

## Repository layout

```
KBC_Host/          host firmware (game, radio, web server, embedded web app)
KBC_Node/          USB-powered buzzer firmware
KBC_BatteryC6/     low-power battery buzzer firmware (XIAO ESP32-C6, beta)
KBC_BatteryC3/     low-power battery buzzer firmware (ESP32-C3 SuperMini, beta)
web/index.html     the web app source (single file, works offline)
tools/embed.ps1    packs web/index.html into KBC_Host/web_index.h (gzip)
docs/              user guide, hardware guide, images
```

## Project status

| Part | Status |
|---|---|
| Host + USB buzzers | Tested on real hardware (3 boards): pairing, rounds, winners, button test, logs |
| Web app | Tested in Chrome/Edge, laptop and phone sizes |
| Battery buzzer (XIAO C6) | **Beta:** firmware builds and pairs; the full press-to-answer path and real battery life still need measuring |
| Battery buzzer (C3 SuperMini) | **Beta:** firmware builds and uploads; pairing, presses and battery life still need testing |

## License

[MIT](LICENSE): free to use, modify and share, including in your own projects.
