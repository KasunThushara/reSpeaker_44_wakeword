/* JARVIS overlay bridge: brain SSE <-> Iron Man hologram UI */
(function () {
  const CMDS = [
    [1, "play some music"], [2, "stop the music"], [3, "move your head"],
    [4, "tell me a joke"], [5, "change the mood"], [6, "show me the weather"],
    [7, "open my tasks"], [8, "shut it down"], [9, "ask the model"]
  ];
  const MOODS = ["cyan", "violet", "amber", "magenta"];

  const root = document.getElementById("jv-root");
  if (!root) return;
  root.innerHTML = `
    <div class="jv-top">
      <div>
        <div class="jv-state" id="jv-state">STANDBY</div>
        <div class="jv-heard" id="jv-heard">\u00a0</div>
      </div>
      <div class="jv-link">
        <span><span class="jv-dot" id="jv-dot"></span><span id="jv-linktext">LINK</span></span>
        <span id="jv-ip"></span>
        <span id="jv-clock"></span>
      </div>
    </div>
    <nav class="jv-cmds" id="jv-cmds"></nav>
    <aside class="jv-log">
      <div class="jv-logtitle">EVENT LOG <span id="jv-logstate">—</span></div>
      <div id="jv-loglist"></div>
    </aside>
    <div class="jv-card" id="jv-card">
      <div class="jv-card-head">
        <div class="jv-card-title" id="jv-card-title"></div>
        <div class="jv-card-sub" id="jv-card-sub"></div>
      </div>
      <ul class="jv-card-items" id="jv-card-items"></ul>
    </div>`;

  // mood wash lives outside #jv-root so it can sit *behind* the robot stage
  // (#jv-root is z-index 30; the stage is z-index 5)
  document.body.insertAdjacentHTML("beforeend", '<div id="jv-wash"></div>');

  const $ = s => root.querySelector(s);
  const list = $("#jv-cmds");

  for (const [id, label] of CMDS) {
    const b = document.createElement("div");
    b.className = "jv-cmd";
    b.dataset.id = id;
    b.innerHTML = `<b>${id}</b><span>${label}</span>`;
    b.addEventListener("click", e => { e.stopPropagation(); send(id); });
    list.appendChild(b);
  }
  addEventListener("keydown", e => {
    const n = parseInt(e.key, 10);
    if (n >= 1 && n <= 9) send(n);
  });
  function send(id) {
    // 400 ms debounce: a double-click must not send two commands
    const now = Date.now();
    if (now - (send._last || 0) < 400) return;
    send._last = now;
    fetch("/api/cmd/" + id).catch(() => {});
  }

  /* ---------- state ---------- */
  // declared before the reactor tick: its IIFE runs immediately
  let st = { log: [], play: false, track: null, mood: 0 };
  let lastMsg = 0;
  let lastFx = 0;
  let lastCardN = 0;
  let cardTimer = 0;
  let headTimer = 0;
  let moodTimer = 0;
  let moodSeen = -1;

  /* ---------- reactor coupling ----------
     The PC plays the audio itself (an isolated player process), so this page
     has no audio signal to analyse. Drive the reactor with a synthetic
     envelope while state.play is true - it looks beat-like and costs nothing. */
  let level = 0;
  (function tick() {
    requestAnimationFrame(tick);
    const hud = window.__hud;
    if (!hud) return;
    if (st.play) {
      const t = performance.now() / 1000;
      const env = 0.34
        + 0.30 * Math.abs(Math.sin(t * 2.1))
        + 0.10 * Math.abs(Math.sin(t * 7.3 + 1.7))
        + 0.04 * Math.abs(Math.sin(t * 17.1));
      level = level * 0.7 + env * 0.3;
      hud.targetSurge = Math.min(0.85, level);
    } else {
      level *= 0.9;
      hud.targetSurge = level < 0.02 ? 0 : level;
    }
  })();

  /* ---------- live state ---------- */
  const es = new EventSource("/api/events");
  es.onopen = () => $("#jv-logstate").textContent = "LINKED";
  es.onerror = () => $("#jv-logstate").textContent = "RECONNECTING";
  es.onmessage = ev => { st = JSON.parse(ev.data); lastMsg = Date.now(); render(); };

  setInterval(() => {
    const on = Date.now() - lastMsg < 6000;
    $("#jv-dot").classList.toggle("on", on);
    $("#jv-linktext").textContent = on ? "LINKED" : "NO LINK";
    $("#jv-logstate").textContent = on ? "LINKED" : "NO LINK";
  }, 1000);

  setInterval(() => {
    $("#jv-clock").textContent = new Date().toTimeString().slice(0, 8);
  }, 500);

  function esc(s) {
    return String(s).replace(/[&<>"]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
  }

  /* ---------- one-shot effects ---------- */
  function playHead() {
    document.body.classList.remove("jv-heading");
    void document.body.offsetWidth;          // restart the animation
    document.body.classList.add("jv-heading");
    clearTimeout(headTimer);
    headTimer = setTimeout(() => document.body.classList.remove("jv-heading"), 2600);
  }

  function playMood() {
    document.body.classList.remove("jv-moodpulse");
    void document.body.offsetWidth;
    document.body.classList.add("jv-moodpulse");
    clearTimeout(moodTimer);
    moodTimer = setTimeout(() => document.body.classList.remove("jv-moodpulse"), 2100);
  }

  function applyMood(mood) {
    const m = Number(mood) || 0;
    if (m === moodSeen) return;
    moodSeen = m;
    for (let i = 0; i < MOODS.length; i++) {
      document.body.classList.toggle("jv-mood-" + i, i === m);
    }
  }

  /* ---------- cards ---------- */
  function renderCard() {
    const c = st.card;
    if (!c || !c.n || c.n === lastCardN) return;
    lastCardN = c.n;

    const el = $("#jv-card");
    clearTimeout(cardTimer);

    if (!c.title || !(c.items || []).length) {
      el.classList.remove("show");
      return;
    }
    $("#jv-card-title").textContent = c.title;
    $("#jv-card-sub").textContent = c.subtitle || "";
    el.dataset.kind = c.kind || "info";

    const ul = $("#jv-card-items");
    ul.innerHTML = "";
    (c.items || []).forEach((it, i) => {
      const li = document.createElement("li");
      if (c.kind === "todo") {
        li.innerHTML = `<span class="n">${i + 1}</span><span class="v">${esc(it)}</span>`;
      } else if (c.kind === "weather") {
        li.innerHTML = `<span class="n">\u25b8</span><span class="v">${esc(it)}</span>`;
      } else {
        li.innerHTML = `<span class="v text">${esc(it)}</span>`;
      }
      ul.appendChild(li);
    });

    el.classList.add("show");
    if (c.ttl > 0) {
      cardTimer = setTimeout(() => el.classList.remove("show"), c.ttl * 1000);
    }
  }

  function render() {
    window.jv = st;
    // an incoming message is the definition of "linked"; update here too so
    // background-tab timer throttling can never show a stale NO LINK
    $("#jv-dot").classList.add("on");
    $("#jv-linktext").textContent = "LINKED";
    $("#jv-logstate").textContent = "LINKED";
    $("#jv-state").textContent = st.status || "STANDBY";
    $("#jv-heard").textContent = st.heard ? `\u201c${st.heard}\u201d` : "\u00a0";
    $("#jv-ip").textContent = st.pc || "";
    document.body.classList.toggle("jv-playing", !!st.play);
    applyMood(st.mood);

    root.querySelectorAll(".jv-cmd").forEach(el => {
      el.classList.toggle("on", Number(el.dataset.id) === st.cmd && !!st.play);
    });

    const l = $("#jv-loglist");
    l.innerHTML = "";
    [...(st.log || [])].reverse().slice(0, 14).forEach(e => {
      const d = document.createElement("div");
      d.className = "jv-le " + (e.kind || "info");
      d.innerHTML = `<span class="t">${e.t}</span><span>${esc(e.text)}</span>`;
      l.appendChild(d);
    });

    // one-shot effects coming from the brain (state.fx)
    if (st.fx && st.fx.n && st.fx.n !== lastFx) {
      lastFx = st.fx.n;
      if (st.fx.name === "head") playHead();
      else if (st.fx.name === "mood") playMood();
    }

    renderCard();

    if (!st.play) {
      const hud = window.__hud;
      if (hud) hud.targetSurge = 0;
    }
  }
})();
