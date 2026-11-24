# HUD: the WALL·E character page

The screen at the booth is a single HTML page served by the brain:
`pc_brain_walle/ui/index.html`. It is a vector WALL·E (inline SVG) with a heads-
up overlay drawn on top. Everything is local — Tailwind and all fonts are in
`ui/vendor/`, so the page works with the internet unplugged.

## 1. Files

| file | role |
| --- | --- |
| `ui/index.html` | the WALL·E character page + overlay includes |
| `ui/walle-overlay.js` | HUD: fetches the registry, renders the 8 cards, consumes SSE, runs the animation engine |
| `ui/walle-overlay.css` | HUD styling |
| `ui/vendor/` | Tailwind build, Fredoka/Nunito (character), Orbitron/Rajdhani (HUD), woff2 files |
| `ui/assets/sfx/` | 12 cue WAVs |

The brain serves the folder as static files with `Cache-Control: no-store`.

## 2. What is on screen

```
┌────────────────────────────────────────────────────────────────────┐
│ WALL·E   IDLE   "do a dance"     VOICE COMMANDS · 8   LINKED  IP  ⌚ │
│                                                                    │
│ ┌───────────────┬───────────────┐                 ┌─────────────┐  │
│ │ 1 Forward     │ 5 Spin        │                 │ EVENT LOG   │  │
│ │ “go straight  │ “spin in      │                 │ 17:04 …     │  │
│ │  ahead”       │  circles”     │                 │ …           │  │
│ ├───────────────┼───────────────┤                 └─────────────┘  │
│ │ 9 Wave        │ 11 Hug        │                                  │
│ ├───────────────┼───────────────┤                 ┌─────────────┐  │
│ │ 19 Dance      │ 29 Charging   │                 │ CARD        │  │
│ ├───────────────┼───────────────┤                 │ (weather)   │  │
│ │ 35 Peekaboo   │ 39 Weather    │                 └─────────────┘  │
│ └───────────────┴───────────────┘                                  │
│                          [ WALL·E character ]                      │
└────────────────────────────────────────────────────────────────────┘
```

- **left**: exactly the eight voice commands, each with the action name, the
  exact phrase to say, and a solid yellow id badge. Clicking a card sends that
  action — useful for testing.
- **right**: the event log (newest first) and a card area used by weather (and
  other data actions in the registry).
- **top bar**: connection state (`LINKED`), the PC's IP, and a clock.

## 3. How the overlay works

1. `GET /api/actions` → the registry; keep only entries with a `voice` field.
2. `GET /api/state` → initial state.
3. Subscribe to `/api/events` (SSE). Each event carries the whole state.
4. On a state change:
   - `status`/`heard` update the top bar
   - `fx.n` changing triggers the animation named `fx.name`
   - `card.n` changing shows the card and hides it after `card.ttl`
   - `log` appends to the event list

The character itself exposes hooks from inside the page's IIFE:

```js
window.__walle = {
  chirp(type), say(text), blink(), wave(), drive(), charge()
}
```

The overlay uses these for small reactions and `#walleStage` transforms for the
big set pieces.

## 4. Animations

`FX` in `walle-overlay.js` maps an action's `fx` name to a function that moves,
tilts, scales or filters the stage, and can pop a speech bubble. The 39 names
include `move_fwd`, `spin`, `wave`, `hug`, `dance`, `charging`, `peekaboo`,
`weather`, `sleep`, `low_battery`, etc.

To add an animation:

1. pick a name, e.g. `jump`
2. register it in the `FX` table in `walle-overlay.js`
3. set `"anim": "jump"` for the action in `brain.py`
4. bump the `?v=` query on the overlay script (see below)

Held states (`sleep`, `low_battery`) are cleared when the next action starts, so
the character cannot get stuck asleep at the booth.

## 5. Cache busting (do not skip)

The page loads the overlay with a version query:

```html
<link rel="stylesheet" href="/walle-overlay.css?v=20261123g">
<script src="/walle-overlay.js?v=20261123g"></script>
```

**Every change to `walle-overlay.js` or `.css` must bump that string**, and the
kiosk needs a reload. Chrome happily keeps an old JS file in a long-lived kiosk
tab; a stale overlay looks exactly like “SSE is broken”.

## 6. Kiosk setup

`start_walle.bat` opens:

```
chrome --kiosk --autoplay-policy=no-user-gesture-required ^
  --disable-pinch --overscroll-history-navigation=0 ^
  --user-data-dir="%TEMP%\walle-kiosk" http://localhost:8080
```

Notes:

- **Alt+F4** exits kiosk; closing the brain window stops UDP
- the HUD is responsive, but it was designed for a 16:9 landscape screen at
  1080p or above; below ~1100 px wide the cards and the character compete for
  space
- `--user-data-dir` isolates the kiosk profile so a visitor's browser state
  cannot interfere
- if the screen sleeps, disable sleep in Windows power settings — a black
  screen at the booth is almost always OS power management, not the app

## 7. Editing the character page

`index.html` is a standalone page (originally `wall_e_interactive_vector_character.html`).
Keep these intact:

- the `window.__walle = …` hook block inside the page's IIFE (the overlay calls
  it)
- `#walleStage` (the overlay transforms this element)
- the page's own three buttons at the bottom — they are part of the character
  page and still work, though they are not voice commands

If you replace the character art, preserve those three identifiers; everything
else is free to change.

## 8. Look and feel rules that were tested

- HUD text sits on a **cream** background in places — dark cards with yellow
  accents stay readable there; pure black text does not
- the eight cards use 20 px action names and 14 px phrases so they are legible
  from 2–3 m; shrinking them makes the booth demo look broken
- red is reserved for the “turn it red” action; errors are **amber**
- the log is capped; it is a debugging aid, not a transcript

## 9. Troubleshooting the HUD only

| symptom | cause | fix |
| --- | --- | --- |
| cards visible, clicks do nothing | brain died or port changed | check the console window, `curl /api/state` |
| old layout after an edit | cached JS | bump `?v=` and reload |
| `LINKED` never appears | SSE blocked | check 8080, browser console, proxy/VPN |
| character frozen mid-animation | an animation threw | open devtools, check console; reload |
| page looks unstyled | `ui/vendor/` missing | restore it from git |

Next: **[Voice Commands](Voice-Commands.md)**.
