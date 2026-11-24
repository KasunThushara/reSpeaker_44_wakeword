# Prerequisites

Everything below was the working set on the reference machine (Windows 11,
November 2026). Newer patch versions are usually fine; the **major** versions
matter.

## 1. Hardware

| item | recommendation | why |
| --- | --- | --- |
| reSpeaker **XVF3800 4-Mic Array 44 mm** | the board with the XIAO socket | mics, DSP, LED ring, speaker out, I²S wiring all on board |
| Seeed **XIAO ESP32-S3 Plus** | 16 MB flash / 8 MB PSRAM | the demo needs the `esp_sr_16` partition (4 MB model partition) |
| USB-C **data** cable | not charge-only | flashing uses the native USB port |
| 5 V USB supply | 1 A or more | the LED ring + speaker draw more than a weak hub port |
| PC with WiFi | Windows 10/11, Linux or macOS all work | runs the brain and the HUD |
| a 2.4 GHz WiFi network | WPA2, **no client isolation** | ESP32-S3 is 2.4 GHz only; isolation blocks PC ↔ device |
| speaker | the 44 mm board's own speaker | cues come out of the robot |

Not required: monitor, keyboard, display module, external microphone.

> The 44 mm board's headers are **2.0 mm pitch**. If you add anything on J5/J6/J2,
> use 2.0 mm connectors or solder short leads — Dupont jumpers on 2.0 mm pins
> make intermittent contact, which produced days of “mysterious” I²C failures.

## 2. Software checklist

| tool | version used | install |
| --- | --- | --- |
| Python | **3.11.9** | `winget install Python.Python.3.11` |
| pip packages | numpy 2.4.6, soundfile 0.14.0, scipy 1.17.1 | see below |
| Arduino CLI | **1.5.1** (bundled with Arduino IDE 2) | `winget install ArduinoSA.CLI` or use the IDE's copy |
| esp32 core | **3.3.11** | `arduino-cli core install esp32:esp32@3.3.11` |
| Chrome or Edge | any current | kiosk HUD |
| Git | 2.50+ | `winget install Git.Git` |
| dfu-util | 0.11+ (only if you must reflash the XVF3800) | `winget install dfu-util` |

Python packages:

```powershell
py -3.11 -m pip install numpy soundfile scipy
```

Verify:

```powershell
py -3.11 -c "import numpy, soundfile, scipy; print(numpy.__version__, soundfile.__version__, scipy.__version__)"
arduino-cli version
arduino-cli core list
```

Expected core list contains `esp32:esp32 3.3.11`.

> **Why 3.3.11?** The ESP-SR library, the `esp_sr_16` partition scheme and the
> `ESP_SR.h` API are tied to the core version. Upgrading the core may require
> revisiting the sketch. The reference build is `3.3.11`.

## 3. Network requirements

The device and the PC must reach each other **in both directions**:

| direction | protocol | port | used for |
| --- | --- | --- | --- |
| XIAO → PC | UDP | **9001** | commands + the ACK goes back to the XIAO's source port |
| PC → XIAO | UDP | **9100** | PCM cues |
| browser → PC | TCP | **8080** | HUD, API, SSE |

What the network must allow:

- both devices in the **same subnet** (the reference setup used
  `192.168.7.0/23` with PC `.217` and XIAO `.205`)
- **no AP/client isolation** — check the router; many guest networks enable it
- no firewall rule blocking inbound UDP 9001 / TCP 8080 on the **private**
  profile

Open the ports once (PowerShell **as Administrator**):

```powershell
New-NetFirewallRule -DisplayName "WALL-E brain HTTP" -Direction Inbound -Protocol TCP -LocalPort 8080 -Action Allow -Profile Private
New-NetFirewallRule -DisplayName "WALL-E brain UDP"  -Direction Inbound -Protocol UDP -LocalPort 9001 -Action Allow -Profile Private
```

Record the PC's IPv4 address — the firmware needs it:

```powershell
ipconfig | Select-String "IPv4"
```

If your site uses DHCP, either reserve the PC's address on the router or plan to
reflash when it changes. (`esp_ip` in `config.json` is discovered automatically
from incoming packets, but the **device** needs `PC_IP` baked in.)

## 4. Accounts and secrets

| service | account needed |
| --- | --- |
| wake word / commands | none — runs offline |
| weather | none — Open-Meteo is keyless |
| anything else | none |

The WiFi SSID/password live in the sketch (`#define WIFI_SSID`, `WIFI_PASS`) and
are therefore visible in the firmware. Use a **separate booth network**, not the
venue's production network.

## 5. Skills that will save you time

You do not need to be an expert, but you should be comfortable with:

- flashing an ESP32 with **Arduino IDE or `arduino-cli`**
- a serial monitor at **115200 baud**
- running a Python script and reading its console
- editing two `#define` lines and a Python dict
- basic networking (`ipconfig`, `ping`, opening a port)

## 6. Time budget

| step | first time | once familiar |
| --- | --- | --- |
| unboxing, reading | 15 min | — |
| XVF3800 firmware check | 10 min (or 40 min if reflashing) | 2 min |
| toolchain install | 20 min | — |
| first firmware build + flash | 10 min | 3 min |
| brain + HUD | 5 min | 1 min |
| phrase/command verification | 15 min | 5 min |
| **total** | **~1.5 h** | **~15 min** |

## 7. Pre-flight check

Before moving on, confirm:

- [ ] XIAO enumerates as a COM port when plugged in
- [ ] `arduino-cli core list` shows `esp32:esp32 3.3.11`
- [ ] `py -3.11 -c "import numpy, soundfile, scipy"` prints nothing
- [ ] PC IPv4 address noted: `________________`
- [ ] WiFi SSID/password noted: `________________`
- [ ] firewall rules created (or firewall disabled on the booth network)
- [ ] the reSpeaker speaker makes a click/pop when powered (codec alive)

Next: **[Assembly](Assembly.md)**.
