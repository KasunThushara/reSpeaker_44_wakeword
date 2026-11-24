# Voice commands

## The eight

| id | you say | WALL·E action | category | cue |
| --- | --- | --- | --- | --- |
| 1 | **go straight ahead** | Forward | Movement | motor whirr |
| 5 | **spin in circles** | Spin | Movement | servo sweep |
| 9 | **wave your hand** | Wave | Social | servo |
| 11 | **give me a hug** | Hug | Social | happy chirp |
| 19 | **do a dance** | Dance | Entertainment | melody |
| 29 | **charge your battery** | Charging | Power | charge whine |
| 35 | **play hide and seek** | Peekaboo | Funny | yes-beep |
| 39 | **show me the weather** | Weather card | Info | happy chirp |

Say the wake word, wait for the chime, then say the phrase **once**. Speaking
during the chime is fine; the listening window opens right after it.

The ids are shared by `firmware/wake_demo/wake_demo.ino`, `brain.py` and the
HUD. They are deliberately sparse and must never be renumbered casually.

## How a phrase becomes an action

```
WakeNet fires               MultiNet searches 6.5 s          PC
     │                            │                          │
"Hi Walle" ──► chime ──► "spin in circles" ──► id 5 ──► JARVIS 7 5 ──► Spin
                                                                 │
                                                        ACK 7 ◄──┘
```

1. **WakeNet** runs continuously on the I²S microphone stream. It is the tuned
   “Hi Walle” model (`wn9_hiwalle_tts2`) from `build/srmodels_hiwalle_sens.bin`.
2. The firmware plays the 0.74 s chime, then switches ESP-SR into MultiNet mode
   for **6.5 s**.
3. **MultiNet** matches against the eight strings compiled into the sketch. It
   returns the **command id** you registered.
4. The sketch looks the id up in `sr_commands[]` (never indexes by it), prints
   `raw id=<n> -> "<phrase>" -> cmd <id>`, and sends the command.
5. If nothing matches in 6.5 s, it sends id `99` so the brain can log “wake
   fired, phrase not matched”.

## What the ring tells you

| ring | meaning |
| --- | --- |
| dim / moving in DOA mode | idle, listening for the wake word |
| bright flash + chime | wake word accepted |
| category colour | command accepted, animation playing |
| **double** amber blink | wake fired, phrase not understood |
| **single** amber blink | phrase understood, PC did not ACK |
| red | the “turn it red” action (never an error) |

## Adding or changing a command

Three places must agree: **id**, **phrase**, **action**.

### 1. Firmware (the phrase)

Edit both arrays in `firmware/wake_demo/wake_demo.ino`:

```cpp
static const sr_cmd_t sr_commands[] = {
  {1,  "go straight ahead"},
  /* … */
  {24, "look around the room"},   // ← new: use an id that exists in brain.py
};

static const char *PHRASE_TEXT[] = {
  "go straight ahead",
  /* … */
  "look around the room",         // ← same order as sr_commands
};
```

Then rebuild, copy the model pack, and reflash
(see [Firmware](Firmware.md#4-put-the-wake-model-into-the-build-directory-required)).
The model pack itself does **not** need to change — MultiNet builds its grammar
from your table at boot.

### 2. Brain (the action)

In `pc_brain_walle/brain.py`:

```python
_a(24, "Look around", "Intelligence", "look_around", "servo_up", "look around the room")
```

The last argument (`voice`) is what makes it appear in the HUD and what the
demo documents as the phrase. Use `None` for API-only actions.

### 3. HUD (only if the animation is new)

If `fx` is an existing animation name, nothing else is needed. Otherwise add the
function to `FX` in `walle-overlay.js` and bump the `?v=` version.

### 4. Restart and test

Restart the brain; reflash the device. Then say the phrase three times and
watch the serial log for `raw id=24`.

> **Do not add an alias** (two phrases for one id). It shifts MultiNet's grammar
> and steals matches from other phrases. One phrase per id.

## Writing phrases that actually work

MultiNet7 (`mn7_en`) is a small on-device model. It is not a cloud STT. The
rules below come from real failed attempts:

| rule | bad example | why it failed | good replacement |
| --- | --- | --- | --- |
| **3+ words** | “dance” | too short, collides with background speech | “do a dance” |
| **one unique verb per phrase** | “start the music”, “start charging” | shared verbs blur together | “charge your battery” |
| **avoid `-ing` forms** | “start charging now” | the runtime G2P handles `charging` poorly | “charge your battery” |
| **avoid likely OOV words** | “play peekaboo now” | `peekaboo` is not in the lexicon — never matched, silently | “play hide and seek” |
| **no minimal pairs** | “walk forward” vs “wave your hand” | first phoneme decides, they steal each other | “go straight ahead” vs “wave your hand” |
| **no homophone traps** | “show my todo” | `todo` is pronounced “to-do”, not matched | “open my tasks” |
| **avoid a word another phrase starts with** | “show me the weather” + “show my todo” | both start `show` | use a different verb |

Also:

- keep phrases **2–4 syllables of clear speech**, not tongue-twisters
- test each new phrase **on its own** before adding the next
- a phrase that “works once” is not verified — do three attempts

If a phrase never matches, you will see **no packet at all** on the brain and a
`99` after 6.5 s. That is a recognition problem, not a network problem.

## Wake word tuning

The wake model is a TTS-trained “Hi Walle”. Out of the box it was reliable only
at 30–50 cm. Two levers fixed it:

### 1. XVF3800 gains (in the sketch, written at boot)

```cpp
PP_AGCMAXGAIN  (resid 17, cmd 11) 32 → 64
AEC_ASROUTONOFF (33, 35)          → 1
AEC_ASROUTGAIN  (33, 36)          1.0 → 1.5
```

Measured alone: ~7 detections in 130 s — **not enough**.

### 2. Detection threshold in the model pack

The WakeNet info string inside `srmodels_hiwalle_sens.bin` reads:

```
wakenet9l_tts2h12_Hi,Wall,E or Hi,瓦力_3_0.580_0.585
```

The two numbers are the 90 %/95 % thresholds; stock is `0.630_0.635`.
Lowering them gave **9/9 detections** at 50 cm, 1 m and 1.5 m, first try, with
no false wakes during a ~2 minute test.

| build | detections | range |
| --- | --- | --- |
| stock thresholds | 7 / 150 s | 50 cm |
| + XVF3800 gains | ~7 / 130 s | 50 cm–1 m |
| **+ 0.580_0.585** | **9/9** | 1.5 m |

**The trade-off is false accepts.** In a hall with PA music and crowd noise:

- keep `0.580_0.585` and test for 10 minutes with nobody speaking
- if it wakes on noise, move to `0.600_0.605` (middle setting)
- the stock pack (`build/srmodels_hiwalle.bin`) is the conservative rollback

### Editing thresholds

```powershell
py -3.11 tools\srmodel_tool.py extract build\srmodels_hiwalle_sens.bin build\hiwalle_x
# edit build\hiwalle_x\wn9_hiwalle_tts2\_MODEL_INFO_
#   keep the string length identical (0.580 -> 0.600), UTF-8, no trailing newline
py -3.11 tools\srmodel_tool.py pack build\hiwalle_x build\srmodels_hiwalle_sens.bin
```

Then reflash (copy to `build\wake_demo\srmodels.bin`, upload). The boot log
prints the numbers it loaded — verify them rather than trusting the file name.

### Training a different wake word

The pack was produced by Espressif's WakeNet tooling from TTS samples of the
phrase. Replacing it means regenerating the model and repacking with
`tools/srmodel_tool.py`; see the ESP-SR documentation for the model format.
A pre-built “Hi ESP” pack is in `build/srmodels_stock_backup.bin` if you need a
known-good baseline.

## Verification protocol

Run this after any change to phrases, thresholds or gains:

1. Serial monitor open (`tools/read_serial.ps1`), brain running.
2. For each phrase: say “Hi Walle”, wait for the chime, say the phrase.
3. Confirm **all** of:
   - serial: `HI WALLE #n`
   - serial: `raw id=<expected> -> "<phrase>" -> cmd <expected>`
   - brain: `[udp] action <id> -> <Name>`
   - screen: correct animation
   - speaker: a cue (any cue — not silence)
4. Repeat 3× per phrase at the real speaking distance.
5. Leave the room quiet for 5 minutes and confirm no false wakes/actions.

Record the results. `build/walle8_test*.txt` are examples of these runs.

## Known-good and known-bad wordings

| phrase | result |
| --- | --- |
| go straight ahead | ✅ |
| spin in circles | ✅ |
| wave your hand | ✅ |
| give me a hug | ✅ |
| do a dance | ✅ |
| charge your battery | ✅ (after “start charging now” failed) |
| play hide and seek | ✅ (after “play peekaboo now” and “cover your eyes” failed) |
| show me the weather | ✅ |
| start charging now | ❌ `-ing` + shared verb |
| play peekaboo now | ❌ OOV `peekaboo` |
| cover your eyes | ❌ never matched |
| show my todo | ❌ `todo` is unpronounceable to the G2P |

Next: **[Run the Demo](Run-the-Demo.md)**.
