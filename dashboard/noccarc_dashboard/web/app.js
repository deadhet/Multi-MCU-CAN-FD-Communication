"use strict";
/* Draws the dashboard model sent by the Python server (noccarc_dashboard/model.py) every few
 * hundred ms over Server-Sent Events. All decisions (texts, which state is abnormal, how
 * values are formatted) are made in Python, where they are tested; this file only draws.
 *
 * A "cell" is a string, or {t: text, l: level} with level "" (normal), "caution", "fault",
 * "info" or "dim". With m: true an abnormal level gets only a small coloured box before plain
 * text (columns derived from a state already marked in full, and the event log). Text from the
 * device is always set with textContent, never as HTML.
 *
 * Mock mode, for layout review only: index.html?mock=normal|fault|lost loads mock.js from
 * this folder (sample data; the server never serves that file). */

const $ = (id) => document.getElementById(id);

function mark(c) {
  if (c === null || c === undefined) {
    const s = document.createElement("span");
    s.className = "lv-dim";
    s.textContent = "–";
    return s;
  }
  if (typeof c !== "object") {
    return document.createTextNode(String(c));
  }
  if (!c.l) {
    return document.createTextNode(c.t);
  }
  if (c.m && c.l !== "dim") {
    /* Small mark: coloured box with the shape, text plain. */
    const f = document.createDocumentFragment();
    const box = document.createElement("span");
    box.className = `mk mk-${c.l}`;
    f.append(box, document.createTextNode(c.t));
    return f;
  }
  const s = document.createElement("span");
  s.className = c.l === "dim" ? "lv-dim" : `lv lv-${c.l}`;
  s.textContent = c.t;
  return s;
}

function put(el, c) {
  el.replaceChildren(mark(c));
}

/* rows: list of lists of cells; cls: one CSS class per column; groupEvery: rule every n rows. */
function fillRows(tbody, rows, cls, groupEvery) {
  const out = rows.map((row, i) => {
    const tr = document.createElement("tr");
    if (groupEvery && (i + 1) % groupEvery === 0 && i + 1 < rows.length) {
      tr.className = "group-end";
    }
    row.forEach((c, k) => {
      const td = document.createElement("td");
      if (cls[k]) td.className = cls[k];
      td.append(mark(c));
      tr.append(td);
    });
    return tr;
  });
  tbody.replaceChildren(...out);
}

function fillKv(table, rows) {
  const out = rows.map(([k, v]) => {
    const tr = document.createElement("tr");
    const th = document.createElement("th");
    th.textContent = k;
    const td = document.createElement("td");
    td.append(mark(v));
    tr.append(th, td);
    return tr;
  });
  table.replaceChildren(...out);
}

function segment(c, small) {
  const s = document.createElement("span");
  s.className = "seg" + (small ? " small" : "") + (c.l ? ` ${c.l}` : "");
  if (!small) s.textContent = c.t;
  s.title = c.title || c.t;
  return s;
}

function show(prefix, on) {
  $(`${prefix}-wait`).hidden = on;
  $(`${prefix}-body`).hidden = !on;
}

function drawHealth(h) {
  const unit = document.createElement("span");
  unit.className = "unit";
  unit.textContent = "%";
  $("s1-pct").replaceChildren(String(h.pct), unit);
  put($("s1-label"), h.label);
  const fill = $("s1-fill");
  fill.style.width = `${h.pct}%`;
  fill.className = "fill" + (h.label.l ? ` ${h.label.l}` : "");
  const meter = $("s1-meter");
  meter.querySelectorAll(".tick").forEach((t) => t.remove());
  for (const p of [25, 50, 75]) {
    const t = document.createElement("div");
    t.className = "tick";
    t.style.left = `${p}%`;
    meter.append(t);
  }
  $("s1-rule").textContent = h.rule;
  $("s1-online").textContent = h.online;
  $("s1-synced").textContent = h.synced;
  $("s1-nodes").replaceChildren(...h.nodes.map((c) => segment(c, false)));
  $("s1-params").replaceChildren(...h.params.map((group) => {
    const g = document.createElement("span");
    g.className = "grp";
    g.append(...group.map((c) => segment(c, true)));
    return g;
  }));
}

const S2_COLS = ["label", "", "", "num", "num", "", "", "num"];
const S3_COLS = ["mono", "label", "num", "mono", "", "", "", "num"];
const EVENT_COLS = ["mono", "mono", ""];

function render(m) {
  document.body.classList.toggle("stale", m.stale);
  $("h-source").textContent = m.source;
  put($("h-link"), m.link);
  put($("h-health"), m.health_head);
  $("h-uptime").textContent = m.uptime || "–";
  $("h-time").textContent = m.pc_time;

  const snap = m.snapshot;
  show("s1", !!snap);
  show("s2", !!snap);
  show("s3", !!snap);
  if (!snap) return;

  $("s1-note").textContent = snap.note;
  drawHealth(snap.health);
  fillKv($("s1-can"), m.config || [["CONFIG", { t: "not received yet", l: "dim" }]]);
  fillKv($("s1-uart"), m.uart);
  fillRows($("s2-rows"), snap.nodes, S2_COLS, 0);
  fillRows($("s2-events"), m.events.length ? m.events : [["", "", { t: "No events yet", l: "dim" }]], EVENT_COLS, 0);
  fillRows($("s3-rows"), snap.params, S3_COLS, 5);
}

/* The page's own link to the Python server: EventSource reconnects by itself. */
function serverLost() {
  document.body.classList.add("stale");
  put($("h-link"), { t: "DASHBOARD SERVER NOT RUNNING", l: "fault" });
}

function connect() {
  const es = new EventSource("stream");
  es.onmessage = (e) => render(JSON.parse(e.data));
  es.onerror = serverLost;
}

const mock = new URLSearchParams(location.search).get("mock");
if (mock) {
  const s = document.createElement("script");
  s.src = "mock.js";
  s.onload = () => render(window.MOCK[mock]);
  document.head.append(s);
} else {
  if (window.INITIAL) render(window.INITIAL); /* filled at once, before the stream's first message */
  connect();
}
