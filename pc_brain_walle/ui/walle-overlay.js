/* WALL-E HUD: action panel + SSE bridge + character animations */
(function () {
  const root = document.getElementById("whud");
  if (!root) return;

  root.innerHTML = `
    <div class="wh-top">
      <div class="wh-brand">WALL·E</div>
      <div class="wh-status" id="wh-status">IDLE</div>
      <div class="wh-heard" id="wh-heard"></div>
      <div class="wh-voice" id="wh-voice" title="">VOICE COMMANDS · <span id="wh-voicecount">0</span></div>
      <div class="wh-link">
        <span><span class="wh-dot" id="wh-dot"></span><span id="wh-linktext">LINK</span></span>
        <span id="wh-pc"></span>
        <span id="wh-clock"></span>
      </div>
    </div>
    <div class="wh-actions" id="wh-actions"></div>
    <aside class="wh-log">
      <div class="title">EVENT LOG <span id="wh-logstate">—</span></div>
      <div id="wh-loglist"></div>
    </aside>
    <div class="wh-card" id="wh-card">
      <div class="wh-card-head">
        <div class="wh-card-title" id="wh-card-title"></div>
        <div class="wh-card-sub" id="wh-card-sub"></div>
      </div>
      <ul id="wh-card-items"></ul>
    </div>`;

  const $ = s => root.querySelector(s);
  const stage = document.getElementById("walleStage");
  const walle = window.__walle || {};

  /* ---------------- character animation engine ---------------- */
  let animTimer = 0;
  let driveTimer = 0;

  function apply(x, y, r, s, ms, filter) {
    if (!stage) return;
    stage.style.transition =
      "transform " + ms + "ms cubic-bezier(.4,0,.2,1), filter " + ms + "ms ease";
    stage.style.transform =
      "translate(" + x + "px," + y + "px) rotate(" + r + "deg) scale(" + s + ")";
    if (filter !== undefined) stage.style.filter = filter;
  }

  function reset(ms) {
    apply(0, 0, 0, 1, ms === undefined ? 320 : ms, "none");
  }

  function sequence(frames, step, filter, hold) {
    clearTimeout(animTimer);
    step = step || 150;
    let i = 0;
    const tick = function () {
      if (i >= frames.length) {
        if (!hold) reset(300);
        return;
      }
      const f = frames[i++];
      apply(f.x || 0, f.y || 0, f.r || 0, f.s || 1, f.ms || step,
            f.f !== undefined ? f.f : filter);
      animTimer = setTimeout(tick, f.ms || step);
    };
    tick();
  }

  function driveToggle(ms) {
    if (!walle.drive) return;
    walle.drive();
    clearTimeout(driveTimer);
    driveTimer = setTimeout(function () {
      if (walle.drive) walle.drive();
    }, ms);
  }

  function say(t) { if (walle.say) walle.say(t); }
  function chirp(t) { if (walle.chirp) walle.chirp(t); }
  function blink() { if (walle.blink) walle.blink(); }

  const FX = {
    move_fwd: function () {
      driveToggle(800);
      sequence([{ y: -22 }, { y: -48 }, { y: -70 }], 150);
      say("Forward!");
    },
    move_back: function () {
      driveToggle(800);
      sequence([{ y: 20 }, { y: 45 }, { y: 70 }], 150);
      say("Backing up");
    },
    turn_left: function () {
      sequence([{ r: -4, x: -18 }, { r: -7, x: -44 }, { r: -8, x: -60 }], 140);
      say("Left!");
    },
    turn_right: function () {
      sequence([{ r: 4, x: 18 }, { r: 7, x: 44 }, { r: 8, x: 60 }], 140);
      say("Right!");
    },
    spin: function () {
      sequence([{ r: 120, ms: 260 }, { r: 240, ms: 260 }, { r: 360, ms: 300 }], 260);
      say("Wheee!");
    },
    stop: function () {
      sequence([{ x: -6, ms: 60 }, { x: 6, ms: 60 }, { x: -3, ms: 60 }, { x: 0, ms: 60 }], 70);
      say("Halt!");
    },

    hello: function () { say("WALL-E!"); chirp("happy"); blink(); },
    goodbye: function () { say("Evaaa?"); if (walle.wave) walle.wave(); reset(400); },
    wave: function () { if (walle.wave) walle.wave(); say("Hello!"); },
    handshake: function () { sequence([{ x: -18, s: 1.03 }, { x: -26, s: 1.04 }], 220); say("Shake!"); },
    hug: function () { sequence([{ s: 1.05 }, { s: 1.09 }], 260); say("Hug!"); },
    kiss: function () { sequence([{ s: 1.04 }, { s: 1.0 }], 220); say("❤"); blink(); },

    happy: function () { sequence([{ y: -14 }, { y: 0 }, { y: -14 }, { y: 0 }], 150, "brightness(1.12)"); say("Yay!"); },
    sad: function () { sequence([{ y: 8, ms: 350 }, { y: 14, ms: 350 }], 350, "grayscale(.45) brightness(.85)", true); say("Aww..."); },
    angry: function () { sequence([{ r: -3 }, { r: 3 }, { r: -3 }, { r: 3 }], 90, "sepia(.35) saturate(1.4) hue-rotate(-18deg)", true); say("Grrr!"); },
    shy: function () { sequence([{ r: -5, x: -8 }, { r: -6, x: -12 }], 300, "brightness(1.06)", true); say("Eep?"); },
    surprised: function () { sequence([{ s: 1.10 }, { s: 1.04 }], 180); say("!"); blink(); },
    confused: function () { sequence([{ r: 4, y: -6 }, { r: 5, y: -8 }], 300); say("Hmm?"); },

    dance: function () {
      sequence([{ r: -5, y: -8 }, { r: 5, y: 0 }, { r: -5, y: -8 }, { r: 5, y: 0 },
                { r: -5, y: -8 }, { r: 5, y: 0 }], 200, "brightness(1.08)");
      say("♪ ♫ ♪");
    },
    sing: function () { sequence([{ s: 1.04 }, { s: 1.0 }, { s: 1.04 }], 320); say("♪ ♫ ♪"); },
    celebrate: function () { sequence([{ y: -24, s: 1.05 }, { y: 0 }, { y: -24 }, { y: 0 }], 180); say("Ta-da!"); },
    clap: function () { sequence([{ s: 1.05 }, { s: 1.0 }, { s: 1.05 }], 160); say("Clap clap!"); },
    robot: function () { sequence([{ r: -2 }, { r: 2 }, { r: -2 }, { r: 0 }], 110); say("BEEP BOOP"); },

    listen: function () { sequence([{ r: 2, y: -4 }, { r: 3, y: -6 }], 300, "brightness(1.05)", true); say("?"); blink(); },
    think: function () { sequence([{ r: -2, y: -8 }, { r: -3, y: -10 }], 400); say("..."); },
    search: function () { sequence([{ x: -50 }, { x: 50 }, { x: -30 }, { x: 30 }], 380); say("Searching"); blink(); },
    look_around: function () { sequence([{ x: -60, r: -3 }, { x: 60, r: 3 }, { x: 0 }], 420); say("?"); blink(); },
    follow: function () { driveToggle(1600); sequence([{ x: -30 }, { x: 30 }, { x: 0 }], 480); say("Coming!"); },

    charging: function () { if (walle.charge) walle.charge(); say("Charging..."); sequence([{ s: 1.02 }], 300, "brightness(1.06)", true); },
    low_battery: function () { sequence([{ y: 10, ms: 400 }], 400, "brightness(.7) grayscale(.3)", true); say("Low power..."); },
    full_charge: function () { sequence([{ s: 1.05 }], 260, "brightness(1.14)", true); say("100%!"); },
    power_on: function () { sequence([{ s: 0.96, f: "brightness(.5)" }, { s: 1.02, f: "brightness(1.1)" }, { s: 1, f: "brightness(1)" }], 260); say("WALL-E!"); },
    sleep: function () { sequence([{ y: 10, ms: 600 }], 600, "brightness(.4) blur(.6px)", true); say("Zzz..."); },

    shake_head: function () { sequence([{ r: 8 }, { r: -8 }, { r: 7 }, { r: -7 }, { r: 0 }], 110); say("No no."); },
    peekaboo: function () { say("Peekaboo!"); blink(); sequence([{ s: 1.06 }, { s: 0.98 }, { s: 1 }], 180); },
    funny_walk: function () { driveToggle(1400); sequence([{ r: 6, y: -6 }, { r: -6, y: 0 }, { r: 6, y: -6 }, { r: -6, y: 0 }], 200); say("Waddle waddle"); },
    tired: function () { sequence([{ y: 12, ms: 500 }], 500, "brightness(.8)", true); say("Phew..."); },
    yawn: function () { sequence([{ s: 1.06, ms: 400 }, { s: 0.99, ms: 400 }], 400); say("Yaaawn"); },

    weather: function () { blink(); }
  };

  function playFx(name) {
    // clear a held state (sleep / low battery) whenever a new action starts
    apply(0, 0, 0, 1, 220, "none");
    const fn = FX[name];
    if (fn) { try { fn(); } catch (e) {} }
    else blink();
  }

  /* ---------------- the 8 voice commands (no categories) ---------------- */
  let ACTIONS = [];

  function renderActions() {
    const wrap = $("#wh-actions");
    wrap.innerHTML = "";
    ACTIONS.forEach(function (a) {
      const b = document.createElement("div");
      b.className = "wh-act voice";
      b.dataset.id = a.id;
      b.title = 'Say "Hi Walle", then "' + a.voice + '"';
      b.innerHTML = "<b>" + a.id + "</b><span>" + a.name +
        "<em>\u201c" + a.voice + "\u201d</em></span>";
      b.onclick = function (e) { e.stopPropagation(); send(a.id); };
      wrap.appendChild(b);
    });
  }

  function send(id) {
    const now = Date.now();
    if (now - (send._t || 0) < 350) return;
    send._t = now;
    fetch("/api/action/" + id).catch(function () {});
  }

  fetch("/api/actions").then(function (r) { return r.json(); }).then(function (d) {
    ACTIONS = (d.actions || []).filter(function (a) { return a.voice; });
    renderActions();
    $("#wh-voicecount").textContent = ACTIONS.length;
    $("#wh-voice").title = ACTIONS.map(function (a) {
      return a.name + ': "Hi Walle", then "' + a.voice + '"';
    }).join("\n");
  }).catch(function () {});

  /* ---------------- live state ---------------- */
  let st = { log: [], fx: { n: 0 }, card: { n: 0 } };
  let lastMsg = 0, lastFx = 0, lastCardN = 0, cardTimer = 0;

  const es = new EventSource("/api/events");
  es.onmessage = function (ev) { st = JSON.parse(ev.data); lastMsg = Date.now(); render(); };

  function setLink(on) {
    $("#wh-dot").classList.toggle("on", on);
    $("#wh-linktext").textContent = on ? "LINKED" : "NO LINK";
    $("#wh-logstate").textContent = on ? "LINKED" : "NO LINK";
  }

  setInterval(function () { setLink(Date.now() - lastMsg < 6000); }, 1000);
  setInterval(function () {
    $("#wh-clock").textContent = new Date().toTimeString().slice(0, 8);
  }, 500);

  function esc(s) {
    return String(s).replace(/[&<>"]/g, function (c) {
      return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c];
    });
  }

  function renderLog() {
    const l = $("#wh-loglist");
    l.innerHTML = "";
    (st.log || []).slice(-12).reverse().forEach(function (e) {
      const d = document.createElement("div");
      d.className = "wh-le " + (e.kind || "info");
      d.innerHTML = '<span class="t">' + e.t + "</span><span>" + esc(e.text) + "</span>";
      l.appendChild(d);
    });
  }

  function renderCard() {
    const c = st.card;
    if (!c || !c.n || c.n === lastCardN) return;
    lastCardN = c.n;
    clearTimeout(cardTimer);
    const el = $("#wh-card");
    if (!c.title || !(c.items || []).length) { el.classList.remove("show"); return; }
    $("#wh-card-title").textContent = c.title;
    $("#wh-card-sub").textContent = c.subtitle || "";
    el.dataset.kind = c.kind || "info";
    const ul = $("#wh-card-items");
    ul.innerHTML = "";
    (c.items || []).forEach(function (it, i) {
      const li = document.createElement("li");
      li.innerHTML = '<span class="n">' + (c.kind === "weather" ? "\u25b8" : i + 1) +
        '</span><span class="v">' + esc(it) + "</span>";
      ul.appendChild(li);
    });
    el.classList.add("show");
    if (c.ttl > 0) cardTimer = setTimeout(function () { el.classList.remove("show"); }, c.ttl * 1000);
  }

  function render() {
    window.walleState = st;
    $("#wh-status").textContent = st.status || "IDLE";
    $("#wh-heard").textContent = st.heard ? "\u201c" + st.heard + "\u201d" : "";
    $("#wh-pc").textContent = st.pc || "";
    setLink(true);
    renderLog();
    renderCard();

    root.querySelectorAll(".wh-act").forEach(function (el) {
      el.classList.toggle("on", Number(el.dataset.id) === st.action);
    });

    if (st.fx && st.fx.n && st.fx.n !== lastFx) {
      lastFx = st.fx.n;
      playFx(st.fx.name);
    }
  }
}());
