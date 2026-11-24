# Firmware: build and flash the XIAO

The demo firmware is a single sketch: `firmware/wake_demo/wake_demo.ino`
(plus `wake_sound.h` and `wake_types.h`). It contains:

- WiFi + UDP client to the PC
- I²S full-duplex setup (16 kHz, 32-bit, stereo, slave)
- ESP-SR: WakeNet (wake word) + MultiNet (the 8 phrases)
- AIC3104 volume control
- XVF3800 audio tuning and the LED ring
- a 3-second PSRAM buffer for PC-streamed cues

## 1. Install the toolchain

Either use **Arduino IDE 2** (which bundles `arduino-cli`) or a standalone CLI.
On the reference machine the bundled CLI is at:

```
C:\Users\seeed\AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe
```

Install the ESP32 core **3.3.11** explicitly:

```powershell
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.11
```

`ESP_SR`, `ESP_I2S`, `WiFi`, `Wire` all come from that core — no Library Manager
step is needed.

Check the board is visible:

```powershell
arduino-cli board list
```

You want a line like `COM9  esp32  ESP32S3 Dev Module`.

## 2. Configure your network (two `#define`s and one IP)

Open `firmware/wake_demo/wake_demo.ino` and edit the block at the top:

```cpp
#define WIFI_SSID ""                 // ← your 2.4 GHz network
#define WIFI_PASS ""                 // ← your password
#define PC_IP     "192.168.7.217"    // ← the PC running brain.py
#define PC_PORT   9001
```

Rules:

- **Fill in your own credentials.** The repository ships with empty strings so
  no one's network is published. With either string empty the sketch prints
  `WiFi: no credentials - edit WIFI_SSID / WIFI_PASS` and boots **local-only**:
  wake word, chime and LED feedback work, nothing reaches the PC.
- **2.4 GHz only** — the ESP32-S3 has no 5 GHz radio.
- `PC_IP` must be the PC's LAN address *at boot time*. Reserve it in DHCP so it
  does not move. (The **brain** learns the XIAO's address automatically; only
  the device needs a hard-coded PC address.)
- The PC and the XIAO must be in the same subnet with no client isolation.

If you change the PC's address later, reflash. There is no settings portal.

## 3. Build

The FQBN is not optional — the generic ESP32-S3 definition is required because
the installed core has no `XIAO_ESP32S3_Plus` board entry, and only the
`esp_sr_16` scheme has the 4 MB model partition.

```powershell
$cli = "C:\Users\seeed\AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
$fqbn = "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=esp_sr_16,CDCOnBoot=cdc"

cd D:\wakeword_anim_demo
& $cli compile --fqbn $fqbn --output-dir build\wake_demo firmware\wake_demo
```

Why each option:

| option | reason |
| --- | --- |
| `FlashSize=16M,PSRAM=opi` | XIAO ESP32-S3 **Plus**: 16 MB flash, 8 MB octal PSRAM (used for the audio ring) |
| `PartitionScheme=esp_sr_16` | creates the ~4 MB **model** partition for the 3.3 MB ESP-SR pack |
| `CDCOnBoot=cdc` | `Serial` must come out of the native USB port, because GPIO43 (UART0 RX) is the I²S data pin |

A successful build prints something like:

```
Sketch uses 2058975 bytes (65%) of program storage space.
```

With the credentials left empty the linker drops the unused WiFi stack, so the
same sketch reports only ~50 %; fill in your SSID/password and it returns to
~65 %.

## 4. Put the wake model into the build directory (required!)

The core's post-build hook copies `srmodels.bin` **only** if it finds
`{build.path}/libraries/ESP_SR` — but the ESP-SR library is compiled in place,
so the copy silently never happens and the upload would flash no model (the
board then boots with the factory “Hi ESP” pack).

Copy the tuned pack in by hand before uploading:

```powershell
Copy-Item build\srmodels_hiwalle_sens.bin build\wake_demo\srmodels.bin -Force
```

| pack | thresholds | use |
| --- | --- | --- |
| `build/srmodels_hiwalle_sens.bin` | `0.580_0.585` | **the one flashed** — works to 1.5 m |
| `build/srmodels_hiwalle.bin` | `0.630_0.635` | stock sensitivity, rollback if false wakes |
| `build/srmodels_jarvis.bin` | `0.627_0.632` + Jarvis | legacy JARVIS demo |
| `build/srmodels_stock_backup.bin` | factory | “Hi ESP”, last-resort rollback |

The pack contains the wake word, the MultiNet English model, the VAD and the
FST built from your command table. **Changing the phrase table requires a
reflash**, but it does *not* require a new model pack.

## 5. Flash

```powershell
& $cli upload -p COM9 --fqbn $fqbn --input-dir build\wake_demo
```

Replace `COM9` with your port. The upload writes the app, the bootloader, the
partition table **and the model** from the build directory. First flash takes
20–40 s, later ones ~15 s.

> Always compile → copy the model → upload as one sequence. Running `upload`
> against a stale build directory flashes a stale app *and* a stale model.

## 6. Watch it boot

```powershell
powershell -ExecutionPolicy Bypass -File tools\read_serial.ps1 -Port COM9 -Seconds 30
```

The helper never asserts DTR/RTS (that combination puts the board in download
mode). Expected:

```
=== wake_demo walle: Hi Walle -> chime -> action -> PC + LED -> DOA ===
WiFi: joining <your-ssid>....
WiFi: ip=192.168.7.205 rssi=-52 dBm -> PC 192.168.7.217:9001
i2s full duplex: ok
audio: ring 96000 bytes (ok)
audio tuning: AGCMAXGAIN=64, ASROUTGAIN=1.5, ASR on
MC Quantized wakenet9: wakenet9l_tts2h12_Hi,Wall,E ... _0.580_0.585
ESP_SR: ok
ready - say Hi Walle, then a command right away
```

What matters:

- `WiFi: ip=...` — if it fails, fix credentials/band first
- the **threshold numbers** in the model line must match the pack you meant to
  flash (`0.580_0.585`, not `0.630_0.635`)
- `ESP_SR: ok` — if this hangs or crashes, the model copy step was skipped

Say **“Hi Walle”**. Serial should print `HI WALLE #1` and the ring should chime.

## 7. The phrase table

At the top of the sketch:

```cpp
static const sr_cmd_t sr_commands[] = {
  {1,  "go straight ahead"},
  {5,  "spin in circles"},
  {9,  "wave your hand"},
  {11, "give me a hug"},
  {19, "do a dance"},
  {29, "charge your battery"},
  {35, "play hide and seek"},
  {39, "show me the weather"},
};

static const char *PHRASE_TEXT[] = { /* same order, for logs */ };
```

- the number is the **action id** shared with `brain.py` and the HUD; it is
  **not** an array index
- the sketch matches the id returned by MultiNet against this table and rejects
  unknown ids — adding a phrase with an id that does not exist in the brain is
  harmless but does nothing
- `PHRASE_TEXT` must stay index-parallel (used in the log line only)

After editing, rebuild, copy the model, reflash. Then verify with the brain
running — see [Voice Commands](Voice-Commands.md#adding-or-changing-a-command).

## 8. Tuning knobs in the sketch

| knob | default | effect |
| --- | --- | --- |
| `MUSIC_PERCENT` | 70 | playback level for cues and the wake chime |
| `DUCK_PERCENT` | 30 | level while listening for a command |
| listening window | `6500` ms | how long MultiNet searches after the chime |
| wake cooldown | 1.2 s | suppresses the model's double trigger |
| `PP_AGCMAXGAIN` | 64 | XVF3800 comms-path AGC ceiling |
| `AEC_ASROUTGAIN` | 1.5 | XVF3800 ASR beam output gain |

Do not lower the listening window below **6 s**: MultiNet's own search timeout
is 5.8 s, and a 5 s window silently cut real matches.

## 9. Using the Arduino IDE instead of the CLI

Works, with two caveats:

1. **Board settings**: select *ESP32S3 Dev Module*, then set Flash Size 16 MB,
   PSRAM **OPI PSRAM**, Partition Scheme **ESP SR 16MB**, USB CDC On Boot
   **Enabled**. These must match the FQBN exactly.
2. **Model**: the IDE has the same post-build copy bug. After *Verify*, copy
   `build/srmodels_hiwalle_sens.bin` into the IDE's build output
   (`%TEMP%\arduino\sketches\...\srmodels.bin`), then upload. The CLI route is
   less error-prone because the build directory is fixed.

## 10. Common build/flash failures

| symptom | cause | fix |
| --- | --- | --- |
| `Sketch too big` | wrong partition scheme | use `PartitionScheme=esp_sr_16` |
| boots, no serial output | `CDCOnBoot` not `cdc` | set it, reflash |
| `ESP_SR: begin failed` / crash right after boot | model missing from the build dir | copy `srmodels.bin` into `build\wake_demo` |
| wake word is “Hi ESP” | stock model was flashed | repeat step 4, check the boot log thresholds |
| `Failed to connect` | port busy / wrong COM | close serial monitors, run `arduino-cli board list` |
| uploads but nothing changes | stale build directory | delete `build\wake_demo` and rebuild |

Next: **[PC Brain](PC-Brain.md)**.
