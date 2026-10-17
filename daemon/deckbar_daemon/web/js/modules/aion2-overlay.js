/* Modul AION 2 · Overlay: Programm (Dienst, Modus), Widgets, Event-Timer. */
import { fill, el, icon, api, S, on, field, input, select, button, chip, callout, card, settingsRow, sw, saver, toast, clampInt } from "../core.js";

const FX = { "": "aus", glanz: "Glanz", brennen: "Brennen", gold: "Gold", frost: "Frost", holo: "Holo", leere: "Leere" };

async function cmd(c) {
  try {
    await api("POST", "/api/overlay/command", { cmd: c });
    toast({ edit: "Bearbeiten-Modus umgeschaltet", toggle: "Overlay umgeschaltet", lock: "Gesperrt (klickdurchlässig)", click: "Entsperrt", reload: "Neu geladen" }[c] || "Erledigt");
  } catch (e) { toast(e.message, true); }
}

async function refresh() {
  try { S.overlay = await api("GET", "/api/overlay"); } catch { /* bleibt */ }
}

function unitChip(o) {
  const u = o?.unit || {};
  if (u.installed === false) return chip("nicht eingerichtet", "warn");
  if (u.active) return chip("läuft", "ok");
  if (u.state === "inactive" && !o.game) return chip("wartet auf AION 2");
  if (u.state) return chip(u.state === "inactive" ? "beendet" : u.state, u.state === "failed" ? "bad" : "");
  return chip("…");
}

function controlTab(ctx) {
  const box = el("div", { class: "stack" });
  const save = saver("PUT", "/api/overlay", (res) => { S.overlay = res; }, 300, "Overlay gespeichert");
  const draw = () => {
    const o = S.overlay;
    if (!o) return;
    const L = o.layout;
    const screens = { "": "Hauptmonitor (laut KDE)", ...Object.fromEntries((o.screens || []).map((s) => [s.name, `${s.name} · ${s.width}×${s.height}${s.primary ? " · Hauptmonitor" : ""}`])) };
    if (L.screen && !(L.screen in screens)) screens[L.screen] = L.screen;
    fill(box,
      o.unit?.installed === false ? callout("warn", "alert", el("span", {}, "Das Overlay-Programm ist noch nicht eingerichtet. Einmal im Repo ausführen: ", el("code", { text: "overlay/install.sh" }), " (braucht pyside6, qt6-webengine, layer-shell-qt).")) : null,
      el("div", { class: "cols" },
        card("Overlay", "Startet mit AION 2 und läuft dann als eigenes Programm im Tray", unitChip(o),
          el("div", { class: "row wrap" },
            button("Bearbeiten", () => cmd("edit"), "primary", "cursor-move"),
            button("Zeigen/Verbergen", () => cmd("toggle"), "", "eye"),
            button("Sperren", () => cmd("lock"), "", "lock"),
            button("Entsperren", () => cmd("click"), "", "lock-open"),
            button("Neu starten", async () => {
              try { S.overlay = await api("POST", "/api/overlay/restart"); toast("Overlay startet neu"); draw(); } catch (e) { toast(e.message, true); }
            }, "ghost", "restart")),
          el("p", { class: "hint small", style: "margin-top:14px" },
            "Im Bearbeiten-Modus Widgets ziehen, Mausrad = Größe, Esc = fertig. Das Meter selbst lässt sich jederzeit an Kopf- oder Fußzeile verschieben. ",
            "Für Tastenkürzel: Systemeinstellungen → Tastatur → Kurzbefehle → Neu → Anwendung → „NexBoard Overlay ein/aus“ bzw. „… bearbeiten“.")),
        card("Darstellung", null, null,
          el("div", { class: "grid" },
            field("Monitor", select(screens, L.screen, (v) => { L.screen = v; save({ screen: v }); }), "Wirkt nach „Neu starten“."),
            field("Beim Start", select({ click: "Meter bedienbar", lock: "Gesperrt (alles klickt durch)" }, L.start_mode, (v) => { L.start_mode = v; save({ start_mode: v }); })),
            field("Namenseffekt", select(FX, L.fx, (v) => { L.fx = v; save({ fx: v }); }), "Auf deinem Namen in der eigenen Meter-Zeile.")),
          settingsRow("Nur bei aktivem Spiel zeigen", "Blendet das Overlay aus, sobald ein anderes Fenster als AION 2 den Fokus hat (KDE)", sw(L.hide_unfocused, (v) => { L.hide_unfocused = v; save({ hide_unfocused: v }); })),
          settingsRow("GPU-Beschleunigung", "Aus nur bei Darstellungsfehlern; wirkt nach Neustart", sw(L.gpu, (v) => { L.gpu = v; save({ gpu: v }); })))),
      callout("info", "information-outline", el("span", {}, "Unter KDE Plasma (Wayland) liegt das Overlay als eigene Ebene über dem Vollbildspiel und nimmt nur dort Klicks an, wo das Meter ist. Ohne layer-shell (GNOME, X11) läuft automatisch das einfache Meter-Fenster.")));
  };
  ctx.onUnmount(on("overlay", draw));
  refresh().then(draw);
  draw();
  return box;
}

function widgetsTab(ctx) {
  const box = el("div", { class: "stack" });
  const save = saver("PUT", "/api/overlay", (res) => { S.overlay = res; }, 400, "Widgets gespeichert");
  const draw = () => {
    const o = S.overlay;
    if (!o) return;
    const w = o.layout.widgets;
    const set = (id, patch) => { Object.assign(w[id], patch); save({ widgets: { [id]: patch } }); };
    const common = (id) => [
      settingsRow("Deckkraft", `${Math.round(w[id].opacity * 100)} %`, el("input", { type: "range", min: 20, max: 100, value: Math.round(w[id].opacity * 100), style: "max-width:200px", oninput: (e) => { set(id, { opacity: Number(e.target.value) / 100 }); e.target.closest(".settings-row").querySelector(".txt span").textContent = `${e.target.value} %`; } })),
      settingsRow("Position zurücksetzen", "An den Standardplatz, Größe 100 %", button("Zurücksetzen", () => { set(id, { x: null, y: null, scale: 1 }); toast("Zurückgesetzt"); }, "sm", "refresh")),
    ];
    fill(box,
      el("div", { class: "cols" },
        card("DPS-Meter", "Oberfläche des A2Tools-Meters", sw(w.meter.enabled, (v) => set("meter", { enabled: v })), ...common("meter")),
        card("Boss-Leiste", "HP des Ziels mit Kill-Prognose", sw(w.boss.enabled, (v) => set("boss", { enabled: v })),
          settingsRow("Kill-Prognose", "Restzeit aus dem HP-Verlauf der letzten 40 s", sw(w.boss.show_ttk, (v) => set("boss", { show_ttk: v }))),
          settingsRow("Außerhalb von Kämpfen ausblenden", null, sw(w.boss.hide_idle, (v) => set("boss", { hide_idle: v }))),
          ...common("boss"))),
      eventsCard(w, set));
  };
  // nicht neu aufbauen, solange ein Regler oder Feld in Benutzung ist (sonst bricht das Ziehen ab)
  const busy = () => box.contains(document.activeElement) && /INPUT|SELECT|TEXTAREA/.test(document.activeElement.tagName);
  ctx.onUnmount(on("overlay", () => { if (!busy()) draw(); }));
  if (!S.overlay) refresh().then(draw);
  draw();
  return box;
}

function eventsCard(w, set) {
  const evs = w.timers.events;
  const push = () => set("timers", { events: evs });
  const hhmm = (min) => `${String(Math.floor((min % 1440) / 60)).padStart(2, "0")}:${String(min % 60).padStart(2, "0")}`;
  const days = ["Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"];
  const rows = evs.map((ev, i) => el("tr", {},
    el("td", {}, sw(ev.enabled !== false, (v) => { ev.enabled = v; push(); })),
    el("td", {}, input(ev.name, (v) => { ev.name = v.trim() || ev.name; push(); }, { maxlength: 40, lazy: true })),
    el("td", {}, select({ 60: "stündlich", 120: "alle 2 h", 180: "alle 3 h", 240: "alle 4 h", 360: "alle 6 h", 720: "alle 12 h", 1440: "täglich", 10080: "wöchentlich" }, String(ev.every), (v) => { ev.every = Number(v); ev.at %= ev.every; push(); }, {})),
    el("td", {}, ev.every === 10080 ? select(Object.fromEntries(days.map((d, k) => [k, d])), String(Math.floor(ev.at / 1440)), (v) => { ev.at = Number(v) * 1440 + (ev.at % 1440); push(); }) : null,
      input(hhmm(ev.at), (v) => { const m = /^(\d{1,2}):(\d{2})$/.exec(v.trim()); if (!m) return toast("Zeit als HH:MM", true); ev.at = (ev.every === 10080 ? Math.floor(ev.at / 1440) * 1440 : 0) + (Number(m[1]) * 60 + Number(m[2])) % Math.min(ev.every, 1440); push(); }, { lazy: true, style: "width:80px", title: "erster Start ab Mitternacht" })),
    el("td", {}, input(ev.open, (v) => { ev.open = clampInt(v, 0, 1440, 0); push(); }, { type: "number", min: 0, max: 1440, style: "width:80px" })),
    el("td", {}, select({ local: "Ortszeit", utc: "UTC" }, ev.tz, (v) => { ev.tz = v; push(); })),
    el("td", {}, button(null, () => { evs.splice(i, 1); push(); }, "sm icon ghost", "delete", { title: "Entfernen" }))));
  return card("Event-Timer", "Countdown bis zum nächsten Start, grün solange offen", sw(w.timers.enabled, (v) => set("timers", { enabled: v })),
    el("div", { style: "overflow-x:auto" }, el("table", { class: "list ev-table" },
      el("thead", {}, el("tr", {}, ["", "Name", "Wiederholung", "Start", "offen (min)", "Zeitzone", ""].map((h) => el("th", { text: h })))),
      el("tbody", {}, rows))),
    el("div", { class: "row wrap", style: "margin-top:14px" },
      button("Ereignis hinzufügen", () => { evs.push({ name: "Neues Ereignis", every: 60, at: 0, open: 0, tz: "local", enabled: true }); push(); }, "sm", "plus"),
      el("span", { class: "hint small", text: "Uhrzeiten der Voreinstellungen bitte im Spiel prüfen (Global, Zeitumstellung)." })));
}

export default {
  tabs: [
    { id: "control", label: "Steuerung", icon: "monitor" },
    { id: "widgets", label: "Widgets", icon: "layers" },
  ],
  status: () => (S.overlay?.unit?.active ? "" : "off"),
  card() {
    const o = S.overlay;
    const w = o?.layout?.widgets;
    const active = w ? Object.entries(w).filter(([, v]) => v.enabled).length : 0;
    return el("div", { class: "stack", style: "gap:10px" },
      el("div", { class: "row wrap" }, unitChip(o), o?.layout?.fx ? chip(`Effekt: ${FX[o.layout.fx]}`, "info") : null),
      el("div", { class: "metric" }, el("b", { text: String(active) }), el("span", { text: active === 1 ? "Widget aktiv" : "Widgets aktiv" })));
  },
  render(tab, ctx) {
    return tab === "widgets" ? widgetsTab(ctx) : controlTab(ctx);
  },
};
