# Assembly and board bring-up

The demo needs **no soldering and no external wiring**: the XIAO plugs into the
44 mm board, and the board already connects the microphones, DSP, codec, LED
ring and I²S bus.

## 1. What you are assembling

```
        reSpeaker XVF3800 4-Mic Array 44 mm
        ┌──────────────────────────────────┐
        │   4 × PDM mics                   │
        │        │                         │
        │   XVF3800 (XMOS DSP)             │
        │   ├── I²S master ──────────┐     │
        │   ├── LED ring (24 × WS2812)│     │
        │   └── I²C control (0x2C)    │     │
        │                             ▼     │
        │                        XIAO ESP32-S3 Plus
        │   AIC3104 codec (0x18) ── I²S ──► speaker
        └──────────────────────────────────┘
                    USB-C to PC / 5 V
```

## 2. Physical steps

1. **Seat the XIAO** in the 44 mm board's XIAO headers. If your board came with
   the XIAO already soldered, skip this.
2. **Check the speaker** is connected to the board's speaker connector (the
   44 mm board ships with a small speaker; any 4–8 Ω speaker works).
3. **Connect USB-C** to the **XIAO's** port, not the XMOS port. The XIAO is what
   you flash.
4. Power on. The LED ring should animate briefly (the XVF3800's boot effect).

> The headers are **2.0 mm pitch**. If you attach anything to J2/J5/J6, use
> 2.0 mm connectors or solder short leads. Dupont jumpers “work” until they
> don't, and the failures look like dead chips.

## 3. I²S / I²C map (fixed by the board)

| signal | XIAO pin | GPIO | note |
| --- | --- | --- | --- |
| I²S BCK | D9 | 8 | XVF3800 drives it (XIAO is I²S **slave**) |
| I²S WS (LRCLK) | D8 | 7 | |
| I²S DIN (mic data) | D6 | 43 | also UART0 RX — hence `CDCOnBoot=cdc` |
| I²S DOUT (speaker) | — | 44 | wired on board |
| I²C SDA | D4 | 5 | control bus |
| I²C SCL | D5 | 6 | |
| LED rail enable | — | I²C GPO 33 | via XVF3800 servicer |

I²C addresses on that bus:

| address | device |
| --- | --- |
| `0x18` | AIC3104 audio codec (speaker volume, mute) |
| `0x2C` | XVF3800 control (LED ring, DOA, audio tuning) |

There is **no 3.3 V pin** on J5/J6. If you add a module, take 3.3 V from TP15.

## 4. The XVF3800 firmware must be the 16 kHz I²S build

This is the single most important hardware prerequisite. The demo assumes:

- **16 kHz**, 2 channels, 32-bit I²S frames
- the XVF3800 as I²S **master**
- control I²C enabled (LED ring + DOA)

Seeed publishes the images in the
[`reSpeaker_XVF3800_4MIC_44mm`](https://github.com/respeaker/reSpeaker_XVF3800_4MIC_44mm)
repository under `xmos_firmwares/i2s/`; the one used here is:

```
respeaker_44mm_i2s_c16k2ch_v1.0.0.bin
```

If your board still runs the 48 kHz firmware, the ESP32 will see garbage or
silence. Flash the 16 kHz image following Seeed's official
**“Update firmware”** section for the 44 mm board (XVF3800 safe mode +
`dfu-util`), then re-check with the bring-up sketches below.

> Do not improvise a `dfu-util` command from memory. Follow the vendor page for
> the exact device id and safe-mode sequence; a wrong image leaves the board in
> DFU until you reflash it correctly.

How to tell it worked, once the XIAO runs `firmware/audio_rms`: speaking should
produce RMS values on the **left** channel at 16 kHz, with silence around
20–30 and speech in the hundreds/thousands.

## 5. Bring-up sketches (optional, recommended for a new board)

These sketches exist in the repo and were used to validate each subsystem
before the demo sketch was written. Flash them with the same FQBN as the demo.

| order | sketch | proves |
| --- | --- | --- |
| 1 | `firmware/i2c_scan` | `0x18` and `0x2C` answer on the control bus |
| 2 | `firmware/led_probe` | the ring renders colour/effects via I²C |
| 3 | `firmware/audio_rms` | 16 kHz I²S mic data arrives (left channel) |
| 4 | `firmware/audio_play` | the codec is unmuted and the speaker plays |
| 5 | `firmware/wake_probe` | ESP-SR loads and detects a wake word |

If step 1 fails, stop — nothing above it will work. Check that you are on the
XIAO's USB port and that the board is powered (the ring lights at boot).

## 6. Expected boot behaviour of the final sketch

When `firmware/wake_demo` starts you should see:

- ring powers on and briefly animates
- a short chime is **not** played at boot (only on wake)
- ring settles into dim DOA mode
- serial prints, at 115200:

```
=== wake_demo walle: Hi Walle -> chime -> action -> PC + LED -> DOA ===
i2s full duplex: ok
audio: ring 96000 bytes (ok)
audio tuning: AGCMAXGAIN=64, ASROUTGAIN=1.5, ASR on
MC Quantized wakenet9: wakenet9l_tts2h12_Hi,Wall,E ... _0.580_0.585
ESP_SR: ok
ready - say Hi Walle, then a command right away
```

If you see `WiFi: FAILED`, the credentials or the 2.4 GHz band are wrong — the
device will still wake and blink but cannot reach the PC.

## 7. Mounting for a booth

- Put the board at **chest height**, speaker facing the visitor, mics
  unobstructed. The 44 mm array's beam is broad, but a flat panel in front of
  the ring muffles the mics.
- Keep the PC and the AP in line of sight if possible; the XIAO's antenna is
  small.
- Power from a **dedicated 5 V 1 A** supply, not a laptop hub, so the ring and
  speaker transients do not brown out the board.
- Leave the USB port accessible: recovering a stuck board means unplugging it.

Next: **[Firmware](Firmware.md)**.
