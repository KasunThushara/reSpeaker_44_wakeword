# Running the demo

## The 30-second version

1. Power the reSpeaker board (USB-C, 5 V).
2. On the PC: `pc_brain_walle\start_walle.bat`.
3. Chrome opens the HUD at `http://localhost:8080`.
4. The HUD says `LINKED`. Say **“Hi Walle”**, wait for the chime, say a command.

If `LINKED` is grey, the browser is not receiving SSE — the brain is not
running. If the ring chimes but nothing happens, the device is not reaching the
PC — check the firmware serial log.

## Checklist: the day before

- [ ] both devices on the **booth** WiFi, same subnet, no client isolation
- [ ] PC's IP reserved in DHCP (or reflashed if it changed)
- [ ] firmware boots with **your** WiFi and **your** PC IP
- [ ] model line in the boot log shows the intended thresholds (`0.580_0.585`)
- [ ] all 8 phrases verified 3× at the real distance
- [ ] 10 minutes of background noise → no false wakes (walk around, talk on a
      phone, play music)
- [ ] speaker loud enough with crowd noise; `MUSIC_PERCENT` is 70
- [ ] kiosk screen brightness set, OS sleep disabled
- [ ] backup model packs present: `build/srmodels_*.bin`
- [ ] a laptop or phone hotspot documented as fallback WiFi (requires reflash —
      know this before the event)

## Checklist: 15 minutes before opening

1. Power the board. Ring animates, then settles.
2. Start the brain:
   ```bat
   D:\wakeword_anim_demo\pc_brain_walle\start_walle.bat
   ```
3. Confirm the console shows `actions 39  cues on` and
   `UDP listening on 9001`.
4. Confirm the HUD top bar says **LINKED**.
5. Click a card (e.g. Weather) — the character reacts even if the robot is off.
6. Say one command end-to-end. Check sound from the robot's speaker.
7. Adjust volume with `+` / `-` in the **firmware** serial monitor if needed.
8. Put the character page in full screen; hide the mouse pointer.

## The visitor script

> “This is WALL·E. He can hear you — say **‘Hi Walle’**.”
>
> *(wait for the chime)*
>
> “Now tell him what to do. Try **‘do a dance’**, or **‘show me the weather’**.”

Things that help:

- The visitor must wait for the **chime** before the phrase. Speaking the
  command during the wake word is the most common failure.
- One phrase, not a sentence. “WALL-E can you do a dance?” is not in the
  grammar.
- Stand 0.5–1.5 m away, facing the board. Not behind it, not shouting into it.
- If it misses, say “he didn't catch that — try again”. Do not repeat louder in
  a different pitch; the model is trained on clear TTS-like speech.

## During the event

| situation | do |
| --- | --- |
| missed command | let them retry once; if it misses twice, use a card on screen (visitors like clicking too) |
| false wake from crowd noise | ignore; if frequent, plan a threshold bump at the next break |
| no sound but the animation works | check the brain log for `cue failed`; restart the brain |
| HUD frozen | `Alt+F4`, relaunch `start_walle.bat` |
| brain crashed | close its window, relaunch; the device keeps working (single amber blink until ACK) |
| device offline | power-cycle the board (USB); it rejoins WiFi in ~5 s |
| whole network died | the demo cannot work without WiFi; use the kiosk cards to keep the screen alive |

## Shutdown

1. `Alt+F4` closes kiosk.
2. Close the brain window (or Ctrl+C).
3. Unplug the board **last** — there is no shutdown sequence.

Nothing is written to disk by the demo except `build/serial_log.txt` when you
run the serial helper and `mix_request.json` (legacy brain only).

## Recovering a stuck board

1. Unplug USB, wait 3 s, replug.
2. If it enumerates but does not boot the app, reflash (model copy included).
3. If the port does not appear at all, hold **BOOT** while plugging in — the
   XIAO then enters download mode; flash, then press **RESET**.

## Changing sensitivity at the event

You cannot edit the model pack without a reflash, but you can change the room:

- move the board away from a speaker/PA
- put a soft barrier behind it (wall reflections hurt the beam)
- ask the PA to lower the volume

If false wakes are frequent, switch to `build/srmodels_hiwalle.bin`
(stock thresholds) — you trade range for quiet. Reflash takes ~1 minute.

Next: **[Troubleshooting](Troubleshooting.md)**.
