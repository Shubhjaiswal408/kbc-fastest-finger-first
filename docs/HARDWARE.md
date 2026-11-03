# Hardware guide

- [Parts](#parts)
- [USB buzzer wiring (ESP32 dev board)](#usb-buzzer-wiring-esp32-dev-board)
- [Battery buzzer wiring (XIAO ESP32-C6, beta)](#battery-buzzer-wiring-xiao-esp32-c6-beta)
- [Flashing the firmware](#flashing-the-firmware)
- [Editing the web app](#editing-the-web-app)
- [Troubleshooting](#troubleshooting)

## Parts

| Part | Qty | Notes |
|---|---|---|
| ESP32 dev board (ESP32-WROOM, "ESP32 Dev Module") | 1 for the host + 1 per USB buzzer | any classic ESP32 with USB |
| Push buttons (momentary, 4-leg tactile or arcade) | 4 per buzzer | |
| Jumper wires / breadboard or a small PCB | | solder for classroom use |
| USB chargers or power banks | 1 per board | |
| *Battery buzzer:* Seeed XIAO ESP32-C6 | 1 per buzzer | |
| *Battery buzzer:* 2 x AA holder | 1 per buzzer | lithium AA cells last longest |
| *Battery buzzer:* 3.3 V buck-boost (e.g. TPS63900 module) | 1 per buzzer | recommended, uses the whole battery |

## USB buzzer wiring (ESP32 dev board)

![ESP32 buzzer wiring](images/wiring-esp32.svg)

| Button | Pin |
|---|---|
| A | GPIO 32 |
| B | GPIO 33 |
| C | GPIO 25 |
| D | GPIO 26 |
| Other leg of every button | GND |
| Status LED | on-board LED (GPIO 2) |

No resistors are needed: the firmware turns on the internal pull-ups.

> **4-leg tactile buttons:** the two legs on the same side are always connected inside.
> Use two **diagonally opposite** legs.

Optional active buzzer (beeper): set `BUZZER_PIN` at the top of `KBC_Node.ino`.

## Battery buzzer wiring (XIAO ESP32-C6, beta)

![XIAO C6 battery buzzer wiring](images/wiring-xiao-c6.svg)

| Button | Pin |
|---|---|
| A | D0 (GPIO 0) |
| B | D1 (GPIO 1) |
| C | D2 (GPIO 2) |
| D | **MTDO pad on the back (GPIO 7)** |
| Other leg of every button | GND |
| LED | built-in user LED |
| Power | 2 x AA → (buck-boost 3.3 V) → **3V3** pin, battery − → **GND** |

Only GPIO 0-7 can wake the ESP32-C6 from deep sleep, and on the XIAO only D0-D2 of those are
on the edge, so button D uses the MTDO pad underneath.

**Never connect USB and the batteries at the same time.**

### How the battery buzzer saves power

It sleeps (≈15 µA per Seeed's spec) until a button is pressed, wakes, sends the answer, flashes
the LED and sleeps again: about 0.5 s awake per press. It checks in with the host once an hour
so the app can show it as "🔋 sleeping, ready".

Because it sleeps during the round, it cannot hear "GO". Instead it reports how long ago the
button was pressed. On its first power-up it measures its own wake-up delay once, and the host
uses that to compute the real press moment. A press just before time runs out still counts.

**Estimated battery life** (estimates, not measurements):

| Setup | Estimate |
|---|---|
| 2 x AA wired straight to 3V3 | about 1 year |
| 2 x AA through a 3.3 V buck-boost | about 2+ years |
| Lithium AA through a buck-boost | up to about 3 years |

To check: measure the sleep current with a multimeter in series with the battery.

### Updating the battery buzzer firmware

It is asleep most of the time, so its USB port disappears. Press **RESET**: it stays awake
for 8 seconds, enough to start an upload. Or hold **BOOT** while plugging in USB.

## Flashing the firmware

Arduino IDE 2 with the **esp32 by Espressif** board package, version **3.x**. No extra libraries.

| Sketch | Board setting |
|---|---|
| `KBC_Host` | ESP32 Dev Module |
| `KBC_Node` | ESP32 Dev Module |
| `KBC_BatteryC6` | XIAO_ESP32C6, *USB CDC On Boot: Enabled* |

With `arduino-cli`:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 KBC_Host
arduino-cli upload  --fqbn esp32:esp32:esp32 -p COM20 KBC_Host
```

```bash
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C6:CDCOnBoot=cdc KBC_BatteryC6
```

> **Upload fails with "Wrong boot mode detected"?** Some boards can't enter upload mode by
> themselves. Hold the **BOOT** button while the upload starts.

### Optional: let the host join your Wi-Fi

Copy `KBC_Host/wifi_secrets.example.h` to `KBC_Host/wifi_secrets.h` and fill in your Wi-Fi name
and password. The host then joins that network too, and you can open **http://kbc.local**
from any laptop on it. `wifi_secrets.h` is git-ignored, so your password is never committed.
You can also set this later in the web app (*Settings → Host Wi-Fi*).

## Editing the web app

The whole app is one file, `web/index.html`. Open it directly in a browser to try it in
demo mode. After changing it, rebuild the embedded copy and re-flash the host:

```bash
powershell -ExecutionPolicy Bypass -File tools/embed.ps1
```

## Troubleshooting

| Problem | Fix |
|---|---|
| Buzzer LED keeps double-blinking | It can't find the host: is the host on? Is *Accept new buzzers* on? |
| Button does nothing | Run the button test (*Buzzers* tab). Check the pin and that the other leg goes to GND. |
| Button presses by itself | Loose wire. The button test shows "loose contact"; re-seat or solder it. |
| Web page slow or "host did not answer" | Move closer to the host; reload the page. |
| Host Wi-Fi not visible | Check *Settings → Host Wi-Fi*, or factory-reset the host. |
| "Wrong boot mode" on upload | Hold BOOT during the upload. |
