# Troubleshooting

Debugging is easiest if you find **which hop** is broken before changing
anything.

```
 wake ──► chime ──► phrase ──► UDP ──► brain ──► SSE ──► HUD
  ①         ②         ③        ④       ⑤         ⑥       ⑦
```

| hop | proof it worked | if not |
| --- | --- | --- |
| ① wake | serial `HI WALLE #n`, ring flashes | the model, the mic, or the distance |
| ② chime | you hear it | codec/volume/I²S |
| ③ phrase | serial `raw id=…` | wording or the 6.5 s window |
| ④ UDP | brain `[udp] action …` + `ACK` sent | WiFi, firewall, `PC_IP` |
| ⑤ brain | `[udp] action N -> Name` | registry, exception in the console |
| ⑥ SSE | HUD `LINKED`, log updates | port 8080, browser |
| ⑦ HUD | animation plays | overlay JS, cache |

## 1. The board does not boot / no serial output

| check | action |
| --- | --- |
| port appears? | `arduino-cli board list`; try another USB-C **data** cable |
| port busy? | close Arduino IDE serial monitor, PuTTY, the serial helper |
| no output at all | was the firmware built with `CDCOnBoot=cdc`? Without it `Serial` goes to UART0, which is blocked by the I²S pin |
| boot loop | open the serial helper and read the panic; usually the model was not flashed (see below) |
| completely dead | hold **BOOT**, plug in, flash, press **RESET** |

`ESP_SR: begin failed`, a crash right after `Build fst from commands`, or a boot
loop is almost always **the model file missing from the build directory**.
Repeat:

```powershell
Copy-Item build\srmodels_hiwalle_sens.bin build\wake_demo\srmodels.bin -Force
arduino-cli upload -p COM9 --fqbn "<fqbn>" --input-dir build\wake_demo
```

Confirm the boot log says `wakenet9l_tts2h12_Hi,Wall,E ... _0.580_0.585`.
If it says `Hi,ESP` you flashed the stock pack.

## 2. It never wakes

| check | action |
| --- | --- |
| does the ring react at all? | if the ring is dark, the board may not be running the sketch |
| serial `HI WALLE` while you speak? | if no: pronunciation, distance, noise, model |
| distance | 0.5–1.5 m, facing the board; the tuned pack is not a far-field miracle |
| pronunciation | say it like “hi WALL-ee”, clearly, not “hi wally” shouted |
| background noise | TV/PA near the board masks the wake band |
| gains applied? | boot log must show `audio tuning: AGCMAXGAIN=64, ASROUTGAIN=1.5, ASR on` |
| wrong model? | boot log thresholds — if `0.630_0.635`, reflash the sens pack |
| still weak | regenerate at `0.580_0.585` (see [Voice Commands](Voice-Commands.md#wake-word-tuning)) or move closer |

Note: if WiFi failed (`WiFi: FAILED`), wake and ring still work, but no action
will reach the PC. The serial log is the only place that shows it.

## 3. It wakes (chime) but never recognises the phrase

This is a **recognition** problem. The brain will log
`no command heard (wake fired, phrase not matched)` after 6.5 s, and the ring
does a double amber blink.

| cause | fix |
| --- | --- |
| spoke before/over the chime | wait for the chime, then speak |
| phrase not in the table | check `sr_commands[]` in the flashed sketch |
| wrong id → rejected as unknown | serial prints `raw id=`; the id must exist in `sr_commands[]` |
| wording is hard for the model | 3+ words, unique verb, no `-ing`, no OOV — see the rules table |
| window too short | it is 6.5 s; do not lower it, MultiNet's own timeout is 5.8 s |
| table edited but not reflashed | the command list is compiled in; reflash |
| two phrases for one id | remove the alias — it shifts the grammar |

## 4. Phrase recognised, but nothing happens

The serial log proves recognition (`raw id=… -> cmd …`), so the failure is
**networking**.

| check | command / action |
| --- | --- |
| same subnet? | `ipconfig` on the PC vs the boot log's `WiFi: ip=` |
| can the PC be reached? | `ping <PC_IP>` from another machine |
| port open? | `Test-NetConnection <PC_IP> -Port 8080` (TCP) and check the firewall rule for **UDP 9001** |
| brain bound? | console shows `UDP   listening on 9001` |
| client isolation? | many guest APs block device↔PC; use a normal SSID |
| two brains? | only one process can bind 9001; kill the other |
| wrong `PC_IP` in firmware | it is compiled in — reflash |
| single amber blink | the sketch never got an `ACK`; it retried twice |

The brain **never replies to unparseable packets** — if you see an echo storm,
something else is echoing; find it.

## 5. Action runs, but there is no sound

| check | action |
| --- | --- |
| brain log `cue missing` | the WAV named by the action is not in `ui/assets/sfx/` |
| brain log `cue failed: ...` | `speaker.py` path, Python, or `soundfile` import problem |
| `esp_ip` wrong | check `config.json`; it should be overwritten by the last packet's source |
| device streaming? | serial `audio: N pkts, 0 underruns` while a cue plays |
| volume | codec might be at 30 % after a failed listen; a new cue sends `JV 100`; `+` in serial raises the base level |
| muted by the XMOS | the sketch re-writes the DAC every 5 s; if it stays silent, re-run `firmware/audio_play` |
| no speaker | the 44 mm board's speaker connector |

The cue is **mono 16 kHz**; a 44.1 kHz stereo WAV still works (speaker.py
resamples), but a corrupted file fails in the log.

## 6. HUD problems

| symptom | cause | fix |
| --- | --- | --- |
| never says `LINKED` | SSE not connected | right port? VPN/proxy? open devtools → Network → `events` |
| cards render, log stays empty | SSE blocked but the initial fetch worked | same as above |
| clicks do nothing | brain dead | console window closed? `Invoke-RestMethod /api/state` |
| stale layout | cached JS | bump `?v=` in `index.html`, reload |
| unstyled page | `ui/vendor/` missing | restore from git |
| character frozen | animation exception | devtools console; reload |
| weather card empty | no internet | expected offline; other actions still work |
| screen goes black | Windows sleep | disable sleep / screensaver |

## 7. False wakes

Symptoms: ring flashes/chimes with nobody saying the name, commands appear from
nowhere.

- Move the board away from speakers, PA and HVAC vents.
- Look at the serial log: `HI WALLE #n` with no nearby speech means the model
  is too sensitive.
- Reflash `build/srmodels_hiwalle.bin` (stock thresholds) for a quiet booth, or
  set the middle thresholds 0.600/0.605.
- A false **command** (without a wake) should be impossible; if you see one,
  note the raw id — MultiNet may have matched a background phrase, which means
  the phrase set needs a more distinctive wording.

## 8. Latency feels long

Normal: 0.7 s chime + speech + MultiNet's end-of-utterance detection. Total from
the end of the phrase to the action is usually **under 1 s**; the animation
starts ~1 ms after the ACK.

If it feels slower:

- WiFi RSSI below −70 dBm (`rssi=` in the boot log) adds retries
- a busy PC with many Chrome tabs can delay SSE rendering
- the animation itself can be long (`dance`), which is intentional

## 9. Error and log reference

### Firmware serial

| line | meaning |
| --- | --- |
| `HI WALLE #1` | wake accepted |
| `chime done` | chime finished, entering command mode |
| `listening for a command (6.5 s)` | MultiNet window open |
| `raw id=19 -> "do a dance" -> cmd 19` | recognition result |
| `no command - amber double blink` | window expired, sent id 99 |
| `-> PC  JARVIS 0007 19` | command sent |
| `ACK 7` | PC confirmed |
| `no ACK for seq 7` | retrying the same sequence |
| `audio: N pkts, 0 underruns` | cue stream health |
| `volume: -3.0 dB (DAC 0x06)` | codec level |

### Brain console

| line | meaning |
| --- | --- |
| `[udp] 192.168.7.205 seq 7 -> action 19` | packet arrived |
| `duplicate seq 7 ignored` | retry, correctly dropped |
| `no command heard (wake fired, phrase not matched)` | id 99 arrived |
| `[udp] action 19 -> Dance` | dispatched |
| `cue -> ...` / `cue failed` | audio spawn |
| `weather: ...` | card data |

### Ring

| pattern | meaning |
| --- | --- |
| dim, moving | idle / DOA |
| bright flash | wake |
| category colour | action |
| double amber | wake fired, phrase not matched |
| single amber | no ACK from PC |
| red | “turn it red” action, not an error |

## 10. Mistakes that cost us days (avoid them)

1. **Flashed without copying the model** → stock “Hi ESP”, or a boot crash.
2. **Indexing `sr_commands` by command id** → wrong action for ids above 8.
   Match the id.
3. **5 s listening window** → below MultiNet's 5.8 s timeout, real matches cut.
4. **A second phrase for one id** → shifts the grammar and steals matches.
5. **`-ing` and OOV words** → silent mismatches (`charging`, `peekaboo`,
   `todo`).
6. **Two brains running** → both bind 9001; duplicated/odd behaviour.
7. **Dupont jumpers on 2.0 mm headers** → intermittent I²C, looks like a dead
   chip.
8. **Editing the overlay without bumping `?v=`** → the kiosk shows an old UI.
9. **Laptop sleep / screensaver** → black screen at the worst moment.
10. **`PYTHONIOENCODING` not set** → a curly quote in a log line can kill an
    action thread on a GBK console.

Still stuck? The measured facts and register-level details are in
**[../SYSTEM.md](../SYSTEM.md)**; the debugging chronology is in
**[../PLAN.md](../PLAN.md)**.
