# Development

## Repository layout

```
wakeword_anim_demo/
├─ README.md                  entry point
├─ SYSTEM.md                  deep reference (protocols, registers, measured data)
├─ PLAN.md                    engineering log, facts 1–38
├─ wiki/                      this documentation
│
├─ firmware/
│  ├─ wake_demo/              ← the demo sketch
│  │  ├─ wake_demo.ino        8 phrases, WakeNet/MultiNet, audio tasks, LED ring
│  │  ├─ wake_sound.h         wake chime as PCM
│  │  └─ wake_types.h         shared enums/types
│  └─ …                       ~20 bring-up probes (i2c_scan, audio_rms, led_probe, …)
│
├─ pc_brain_walle/            ← current PC side
│  ├─ brain.py                UDP + HTTP + SSE, 39-action registry, cues
│  ├─ speaker.py              16 kHz PCM streamer to UDP 9100
│  ├─ config.json             esp_ip fallback, weather
│  ├─ start_walle.bat         booth launcher
│  ├─ tools/make_cues.py      cue synthesis
│  └─ ui/                     WALL·E page + overlay + vendor + cues
│
├─ pc_brain/                  legacy JARVIS brain (same ports)
├─ build/                     model packs (committed) + local build output (ignored)
├─ tools/                     srmodel_tool.py, read_serial.ps1, probes
├─ assets/, ui/               source page and raw audio used while building
└─ index.html, walle-overlay.css   empty stubs (harmless, safe to delete)
```

## Git workflow

The repository is initialized on `main`. Build outputs are ignored; **model
packs are committed** because they are hand-tuned artifacts.

Tracked on purpose:

- `build/srmodels_hiwalle_sens.bin` — the flashed pack (0.580/0.585)
- `build/srmodels_hiwalle.bin` — stock-threshold rollback
- `build/srmodels_jarvis.bin`, `build/srmodels_stock_backup.bin`,
  `build/srmodels_wn10.bin` — historical packs
- `build/*.txt` — serial logs from tuning (the evidence behind PLAN facts)

Ignored:

- `firmware/*/build/`, `build/*/` — compiled apps, `.elf`, `.map`, merged images
- `build/hiwalle_x/`, `build/models_*` — extracted model trees
- `__pycache__/`, `*.pyc`, `*.wav` recordings under `build/`
- `pc_brain/tts.wav`, `mix_request.json`
- OS junk (`Thumbs.db`, `.DS_Store`)

Typical loop:

```powershell
git status
git add -A
git commit -m "firmware: raise listening window to 6.5 s"
git push
```

If you change the model packs, commit them — they are **not** reproducible from
source in this repo. 3.3 MB each is small enough for normal Git; no LFS needed.

To add a remote:

```powershell
git remote add origin git@github.com:<user>/wakeword_anim_demo.git
git push -u origin main
```

The wiki pages in `wiki/` are GitHub-wiki-compatible. To publish, enable the
wiki and push:

```powershell
git clone https://github.com/<user>/wakeword_anim_demo.wiki.git
Copy-Item wiki\*.md wakeword_anim_demo.wiki\
cd wakeword_anim_demo.wiki
git add -A; git commit -m "wiki: initial"; git push
```

(`Home.md` and `_Sidebar.md` are recognised automatically.)

## Rebuilding the firmware

```powershell
$cli = "C:\Users\seeed\AppData\Local\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
$fqbn = "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=esp_sr_16,CDCOnBoot=cdc"

& $cli compile --fqbn $fqbn --output-dir build\wake_demo firmware\wake_demo
Copy-Item build\srmodels_hiwalle_sens.bin build\wake_demo\srmodels.bin -Force
& $cli upload -p COM9 --fqbn $fqbn --input-dir build\wake_demo
```

The model copy is not optional. See [Firmware](Firmware.md#4-put-the-wake-model-into-the-build-directory-required)
for why.

## Cue sounds

```powershell
py -3.11 pc_brain_walle\tools\make_cues.py
```

Writes 12 WAVs to `pc_brain_walle/ui/assets/sfx/` at 16 kHz mono. Replace any
file with a real recording of the same name; keep it mono and short (0.3–2 s)
so cues do not step on the next command.

## Adding a whole new action (end to end)

1. **Brain**: add `_a(id, "Name", "Category", "fx_name", "cue_name", None)`.
2. **HUD**: if `fx_name` is new, add its function to `FX` in
   `walle-overlay.js`; bump `?v=` in `index.html`.
3. **Cue**: add a WAV to `ui/assets/sfx/` or reuse an existing cue name.
4. **Firmware** (only if it should be a voice command): add the phrase to
   `sr_commands[]` + `PHRASE_TEXT[]`, reflash.
5. **Test**: click the card in the HUD, then say it.

The registry drives `/api/actions`, so the HUD picks the action up on reload.

## Testing checklist

Before committing anything that touches the demo path:

- [ ] firmware compiles with the FQBN above, app < 65 %
- [ ] boot log shows the correct model thresholds and `ESP_SR: ok`
- [ ] wake word fires from 1.5 m, 3×
- [ ] each changed phrase recognised 3×, with the right action
- [ ] cue audible, no underruns
- [ ] HUD: `LINKED`, correct animation, card if applicable
- [ ] no false wakes in 5 minutes of noise
- [ ] `git status` clean of build artifacts

## Style notes

- **Firmware** is a single sketch on purpose: the ESP-SR example pattern, no
  custom libraries. Keep I²C writes on the main loop (the audio tasks must not
  touch the bus).
- **Brain** is stdlib-only apart from audio decoding. Keep it dependency-light:
  a booth PC should not need a virtualenv rebuild.
- **HUD** is plain ES5-ish JS with no bundler. Edit and reload; bump `?v=`.
- **Ids are forever**: never renumber an action; add a new id.
- **Voice phrases are data**: one phrase per id, no aliases.

## What lives where

| question | file |
| --- | --- |
| why 6.5 s? | `PLAN.md` fact 38, `SYSTEM.md` §4.4 |
| what is the audio packet format? | `SYSTEM.md` §4.6 |
| which registers set the volume? | `SYSTEM.md` §4.7 |
| how do I change the wake threshold? | `Voice-Commands.md` + `tools/srmodel_tool.py` |
| what was tried and failed? | `PLAN.md` (chronological) |
| what does the brain expose? | `PC-Brain.md` §6 |

## Useful one-liners

```powershell
# who is on which port?
Get-NetTCPConnection -LocalPort 8080 | Select-Object OwningProcess
Get-NetUDPEndpoint -LocalPort 9001 | Select-Object OwningProcess

# kill stray brains (inspect first!)
Get-Process python | Select-Object Id,StartTime,Path

# list model packs
py -3.11 tools\srmodel_tool.py list build\srmodels_hiwalle_sens.bin

# capture serial for 30 s
powershell -ExecutionPolicy Bypass -File tools\read_serial.ps1 -Port COM9 -Seconds 30
```
