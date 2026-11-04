// mCOLD Line: the devices of a lot, adding one over USB, the tests, the debug stream.
import { addBox, factoryOn, NeedRom, STEPS } from "./usb.js";

const ICONS = {
  snow: '<path d="M12 2v20M4.9 4.9l14.2 14.2M2 12h20M4.9 19.1L19.1 4.9M9 3.5l3 2.5 3-2.5M9 20.5l3-2.5 3 2.5M3.5 9l2.5 3-2.5 3M20.5 9L18 12l2.5 3"/>',
  play: '<path d="M7 4.5v15l12.5-7.5z"/>', stop: '<rect x="6" y="6" width="12" height="12" rx="2.5"/>',
  reset: '<path d="M3 12a9 9 0 1 0 2.6-6.4L3 8"/><path d="M3 3v5h5"/>', log: '<path d="M4 17l6-5-6-5M12 19h8"/>',
  check: '<path d="M5 12.5l4.5 4.5L19 7.5"/>', x: '<path d="M6.5 6.5l11 11M17.5 6.5l-11 11"/>',
  plug: '<path d="M9 2v5M15 2v5M6 7h12v4a6 6 0 0 1-12 0zM12 17v5"/>', clock: '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>',
  more: '<circle cx="5" cy="12" r="1.2"/><circle cx="12" cy="12" r="1.2"/><circle cx="19" cy="12" r="1.2"/>',
  sun: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>',
  moon: '<path d="M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5z"/>', info: '<circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8h.01"/>',
  list: '<path d="M9 6h11M9 12h11M9 18h11M4 6h.01M4 12h.01M4 18h.01"/>', download: '<path d="M12 4v11M7 10l5 5 5-5M5 20h14"/>',
  plus: '<path d="M12 5v14M5 12h14"/>', trash: '<path d="M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"/>',
  gear: '<circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z"/>',
  flash: '<path d="M13 2L4 14h7l-1 8 9-12h-7z"/>', code: '<path d="M8 6l-6 6 6 6M16 6l6 6-6 6"/>',
  truck: '<path d="M2 6h11v10H2zM13 9h4l4 4v3h-8z"/><circle cx="6" cy="17.5" r="1.8"/><circle cx="17" cy="17.5" r="1.8"/>',
  bug: '<rect x="7" y="7" width="10" height="13" rx="5"/><path d="M12 7V4M9 4l1.5 3M15 4l-1.5 3M3 11h4M17 11h4M3 17h4M17 17h4"/>',
};
const icon = (n) => `<svg class="i" viewBox="0 0 24 24" aria-hidden="true">${ICONS[n] || ""}</svg>`;
const paint = (root) => root.querySelectorAll("[data-icon]").forEach((e) => {
  if (!e.dataset.painted) { e.insertAdjacentHTML("afterbegin", icon(e.dataset.icon)); e.dataset.painted = 1; } });
const $ = (id) => document.getElementById(id);
const text = (el, v) => { if (el.textContent !== v) el.textContent = v; };
const store = (k, v) => { try { localStorage.setItem(k, v); } catch (e) { /* private window */ } };
const load = (k) => { try { return localStorage.getItem(k) || ""; } catch (e) { return ""; } };

const BADGE = { flashing: "กำลัง flash", waiting: "รอ online", online: "พร้อมทดสอบ", running: "กำลังทดสอบ", ask: "รอคนดู",
                pass: "ผ่าน รอส่งมอบ", fail: "ไม่ผ่าน", usb_fail: "flash ไม่ผ่าน", stopped: "หยุดแล้ว", other: "ทำจาก Dock",
                shipping: "กำลังส่งมอบ", shipped: "ส่งมอบแล้ว", ship_fail: "ส่งมอบไม่ผ่าน" };
const NET_MAX = 5;      // the box keeps five networks and joins whichever it finds
let cfg = null, devs = null, filter = load("line.filter"), lastQ = 0, audio = null, busy = false;

function toast(msg, ok) {
  const t = document.createElement("div"); t.className = "toast" + (ok ? " ok" : "");
  t.innerHTML = icon(ok ? "check" : "info"); t.appendChild(document.createTextNode(msg));
  document.body.appendChild(t); setTimeout(() => t.remove(), 5000);
}
async function post(path, body, quiet, method = "POST") {
  try {
    const r = await fetch(path, { method, headers: { "Content-Type": "application/json" }, body: body ? JSON.stringify(body) : undefined });
    const j = await r.json().catch(() => ({}));
    if (!r.ok && !quiet) toast(j.error || `${r.status}`);
    return r.ok ? j : { error: j.error || r.status };
  } catch (e) { if (!quiet) toast("ติดต่อ server ไม่ได้: " + e); return { error: String(e) }; }
}
const get = async (path) => (await fetch(path, { cache: "no-store" })).json();
function beep() {
  try {
    audio = audio || new (window.AudioContext || window.webkitAudioContext)();
    const o = audio.createOscillator(), g = audio.createGain();
    o.frequency.value = 880; g.gain.setValueAtTime(0.0001, audio.currentTime);
    g.gain.exponentialRampToValueAtTime(0.09, audio.currentTime + 0.02);
    g.gain.exponentialRampToValueAtTime(0.0001, audio.currentTime + 0.25);
    o.connect(g); g.connect(audio.destination); o.start(); o.stop(audio.currentTime + 0.26);
  } catch (e) { /* no sound */ }
}
const pad = (n) => n == null ? "?" : String(n).padStart(cfg ? cfg.unit_digits : 3, "0");

/* ---- theme, header ---- */
const root = document.documentElement;
function setTheme(t) { root.dataset.theme = t; store("line.theme", t); $("theme").innerHTML = icon(t === "dark" ? "sun" : "moon"); }
setTheme(load("line.theme") || "dark");
$("theme").onclick = () => setTheme(root.dataset.theme === "dark" ? "light" : "dark");
paint(document);
const op = $("operator"); op.value = load("line.operator"); op.oninput = () => store("line.operator", op.value);
$("menubtn").onclick = (e) => { e.stopPropagation(); $("menu").classList.toggle("open"); };
document.addEventListener("click", () => $("menu").classList.remove("open"));
$("apibtn").onclick = () => window.open("/docs", "_blank");
document.querySelectorAll("dialog [data-close]").forEach((b) => { b.onclick = () => b.closest("dialog").close(); });

const lotIn = $("lot");
async function saveLot() {
  if (!cfg || lotIn.value.trim() === cfg.lot) return;
  const j = await post("/api/lot", { lot: lotIn.value.trim() });
  if (j.error) lotIn.value = cfg.lot; else { await loadConfig(); setUnit(j.next_unit); toast("ล็อต " + cfg.lot, true); }
}
lotIn.onblur = saveLot;
lotIn.onkeydown = (e) => { if (e.key === "Enter") lotIn.blur(); if (e.key === "Escape") { lotIn.value = cfg.lot; lotIn.blur(); } };
lotIn.oninput = () => { lotIn.value = lotIn.value.replace(/\s/g, ""); };
$("auto").onchange = (e) => post("/api/line/auto", { on: e.target.checked, operator: op.value });
$("shipall").onclick = async () => {
  const n = devs ? devs.rows.filter((r) => r.state === "pass" && r.online).length : 0;
  if (!n) { toast("ไม่มีเครื่องที่ผ่านและรอส่งมอบ"); return; }
  if (!confirm(`ส่งมอบ ${n} เครื่อง: ปิดโหมดโรงงาน ใส่ Wi-Fi ลูกค้า (Wi-Fi โรงงานยังอยู่ เป็น DHCP)?`)) return;
  const j = await post("/api/ship/start", { operator: op.value });
  if (!j.error) toast(`เริ่มส่งมอบ ${j.started} เครื่อง`, true);
};
$("testall").onclick = async () => {
  beep(); const j = await post("/api/line/start", { operator: op.value });
  if (!j.error) toast(j.started ? `เริ่มทดสอบ ${j.started} เครื่อง` : "ไม่มีเครื่องที่พร้อมทดสอบ", !!j.started);
};

async function loadConfig() {
  cfg = await get("/api/config");
  if (document.activeElement !== lotIn) lotIn.value = cfg.lot;
  lotIn.placeholder = cfg.lot_example;
  $("lotctl").classList.toggle("empty", !cfg.lot);
  text($("lotchip"), cfg.lot ? "ล็อต " + cfg.lot : "ยังไม่ได้กรอกล็อต");
  text($("subtitle"), (cfg.product_name || "") + (cfg.station ? " · " + cfg.station : ""));
  $("demo").hidden = !cfg.demo;
  $("auto").checked = cfg.auto;
  $("noserial").hidden = cfg.demo || "serial" in navigator;
  renderWifiChoices();
  const fu = cfg.firmware.usb, ff = cfg.firmware.final;
  text($("fwnote"), `Flash: ${fu.ok ? fu.version : "⚠ " + fu.msg} · ตอนจบทดสอบ OTA เป็น: ${ff.ok ? ff.version : "⚠ " + ff.msg}`);
}

/* ---- the devices ---- */
const tiles = new Map();
const ACTIVE = ["running", "ask", "shipping"];
function tile(d) {
  let t = tiles.get(d.sn);
  if (!t) {
    const el = document.createElement("div"); el.className = "tile";
    el.innerHTML = `<div class="th"><span class="u"></span><span class="badge"></span></div>
      <div class="ids"><span class="k">SN</span><span class="v sn"></span><span class="k">MAC</span><span class="v mac"></span>
        <span class="k">IP</span><span class="v ip"></span><span class="k">FW</span><span class="v fw"></span></div>
      <div class="bar"><i></i></div><div class="st"></div>
      <div class="tq" hidden><p></p><div class="qb">
        <button class="yes">${icon("check")}ผ่าน<kbd>Y</kbd></button><button class="no">${icon("x")}ไม่ผ่าน<kbd>N</kbd></button>
        <button class="again" title="สั่งกล่องทำอีกครั้ง (R)">${icon("reset")}<kbd>R</kbd></button></div></div>
      <div class="soak" hidden><div class="sh"><span class="sl"></span><button class="btn sm icon s-view" title="กราฟและสรุป">${icon("list")}</button>
        <button class="btn sm s-stop" title="จบทดสอบนานแล้วสรุปผล">${icon("stop")}จบ</button></div><div class="sv"></div><div class="sp"></div></div>
      <div class="acts"><button class="btn sm a-go">${icon("play")}<span>ทดสอบ</span></button>
        <button class="btn sm a-retest" title="ทดสอบเต็มชุดอีกครั้ง (ส่งมอบแล้ว: เปิดโหมดโรงงานผ่าน USB ก่อน)">${icon("reset")}<span>ทดสอบซ้ำ</span></button>
        <button class="btn sm a-stop">${icon("stop")}หยุด</button>
        <button class="btn sm a-ship" title="ปิดโหมดโรงงาน ใส่ Wi-Fi ลูกค้า (Wi-Fi โรงงานยังอยู่ เป็น DHCP)">${icon("truck")}ส่งมอบ</button>
        <button class="btn sm a-soak" title="อ่านค่ากล่องทุกนาทีจนกว่าจะกดจบ แล้วสรุปผล">${icon("clock")}ทดสอบนาน</button>
        <label class="switch a-dbg" title="เปิด/ปิด stream (client ตัวที่สองของกล่อง → debug broker)"><input type="checkbox">stream</label>
        <span class="tools"><button class="btn sm icon a-view" title="ดู debug: ข้อมูลปกติ, GET_DEBUG, stream">${icon("bug")}</button>
        <button class="btn sm icon a-cfg" title="ตั้งค่าเครื่องนี้">${icon("gear")}</button>
        <button class="btn sm icon a-log" title="ประวัติ">${icon("log")}</button></span></div>`;
    const q = (s) => el.querySelector(s);
    t = { el, u: q(".u"), badge: q(".badge"), sn: q(".sn"), mac: q(".mac"), ip: q(".ip"), fw: q(".fw"), bar: q(".bar i"),
          st: q(".st"), go: q(".a-go"), golbl: q(".a-go span"), stop: q(".a-stop"), dbg: q(".a-dbg"), dbgin: q(".a-dbg input"),
          view: q(".a-view"), log: q(".a-log"), ship: q(".a-ship"), retest: q(".a-retest"), soakb: q(".a-soak"), cfg: q(".a-cfg"),
          tq: q(".tq"), tqp: q(".tq p"), again: q(".tq .again"), soak: q(".soak"), sl: q(".soak .sl"), sv: q(".soak .sv"),
          sp: q(".soak .sp") };
    t.go.onclick = () => { beep(); post("/api/line/start", { sns: [t.d.sn], operator: op.value }); };
    t.retest.onclick = () => retest(t.d);
    t.stop.onclick = () => post(`/api/line/stop/${encodeURIComponent(t.d.sn)}`);
    t.dbgin.onchange = async () => {
      const j = await post(`/api/devices/${encodeURIComponent(t.d.sn)}/debug`, { on: t.dbgin.checked, minutes: 60 });
      if (j.error) t.dbgin.checked = !t.dbgin.checked; else if (t.dbgin.checked) openDebug(t.d);
    };
    t.view.onclick = () => openDebug(t.d);
    t.log.onclick = () => openLog(t.d);
    t.cfg.onclick = () => openCfg(t.d);
    t.ship.onclick = () => { beep(); post("/api/ship/start", { sns: [t.d.sn], operator: op.value }); };
    t.soakb.onclick = async () => {
      const j = await post("/api/soak/start", { sns: [t.d.sn], operator: op.value });
      if (!j.error) toast(`เริ่มทดสอบนาน ${t.d.sn}`, true);
    };
    q(".s-stop").onclick = () => stopSoak([t.d.sn]);
    q(".s-view").onclick = () => openSoak(t.d);
    q(".tq .yes").onclick = () => t.q && answer(t.q, true);
    q(".tq .no").onclick = () => t.q && answer(t.q, false);
    t.again.onclick = () => t.q && answer(t.q, "again");
    tiles.set(d.sn, t);
  }
  t.d = d;
  const q = (devs.questions || []).find((x) => x.sn === d.sn && !answered.has(x.id));
  const state = d.state === "ask" && !q ? "running" : d.state;      // answered: the box carries on
  if (t.el.dataset.state !== state) t.el.dataset.state = state;
  text(t.u, pad(d.unit)); text(t.badge, (BADGE[state] || state) + (d.phase === "retest" && ACTIVE.includes(state) ? " (ซ้ำ)" : ""));
  text(t.sn, d.sn || "—"); text(t.mac, d.mac || "—"); text(t.ip, d.ip || "—"); text(t.fw, d.fw || "—");
  t.bar.style.width = Math.round((d.progress || 0) * 100) + "%";
  const f = d.fail || {};
  text(t.st, ["fail", "usb_fail", "ship_fail"].includes(d.state) ? `${d.phase === "retest" ? "ทดสอบซ้ำ: " : ""}${f.step || ""}${f.detail ? " · " + f.detail : ""}`
           : ACTIVE.includes(d.state) ? (d.step_name || "")
           : d.state === "pass" ? (d.phase === "retest" ? "ทดสอบซ้ำผ่าน" : "ผ่านทุกข้อ") + " · อยู่ Wi-Fi โรงงาน ทดสอบ/debug ต่อได้ จนกว่าจะส่งมอบ"
           : d.state === "shipped" ? "ส่งมอบแล้ว · โหมดโรงงานปิด · มี Wi-Fi ลูกค้า (Wi-Fi โรงงานยังอยู่)"
           : d.state === "waiting" ? "รอกล่องต่อ Wi-Fi + broker"
           : d.state === "online" ? "พร้อมทดสอบ" : d.state === "flashing" ? "กำลังเขียนผ่าน USB" : "");
  t.st.title = f.help || "";
  // the question waiting on this box, answered right here
  t.q = q || null;
  t.tq.hidden = !q;
  if (q) {
    text(t.tqp, q.text);
    t.again.hidden = !q.repeat;
    t.tq.classList.toggle("first", q.id === firstQ());
  }
  // the long test: what it read last and what is not normal so far
  const sk = d.soak;
  t.soak.hidden = !sk || (!sk.running && !sk.result);
  if (sk && sk.running) {
    t.soak.className = "soak run" + (sk.problems.length ? " bad" : "");
    const l = sk.last || {};
    text(t.sl, `ทดสอบนาน ${hm(sk.elapsed)}`);
    t.sl.title = `อ่านแล้ว ${sk.samples} ครั้ง`;
    text(t.sv, [l.online === false ? "offline" : null, l.temp != null ? `${l.temp} °C` : null, l.rssi != null ? `${l.rssi} dBm` : null,
                l.soc != null ? `แบต ${l.soc}%` : null, l.internal != null ? `RAM ${kb(l.internal)}` : null].filter(Boolean).join(" · "));
    text(t.sp, sk.problems.join(" · "));
  } else if (sk && sk.result) {
    t.soak.className = "soak done " + sk.result;
    text(t.sl, `ทดสอบนานล่าสุด: ${sk.result === "PASS" ? "ปกติ" : "ไม่ปกติ"}`);
    text(t.sv, ""); text(t.sp, sk.problems.join(" · "));
  }
  const active = ACTIVE.includes(d.state);
  const retestPhase = d.phase === "retest";
  t.go.hidden = active || !(d.state === "online" || (["fail", "stopped"].includes(d.state) && !retestPhase));
  text(t.golbl, d.state === "online" ? "ทดสอบ" : "ทำซ้ำ");
  t.retest.hidden = active || !d.online || !(["pass", "shipped", "ship_fail"].includes(d.state)
                                              || (["fail", "stopped"].includes(d.state) && retestPhase));
  t.retest.querySelector("span").textContent = ["fail", "stopped"].includes(d.state) ? "ทำซ้ำ" : "ทดสอบซ้ำ";
  t.ship.hidden = !["pass", "ship_fail"].includes(d.state) || !d.online;
  t.stop.hidden = !active;
  t.soakb.hidden = !d.online || !!(sk && sk.running) || d.state === "flashing";
  t.dbg.hidden = !cfg.debug_available || !d.online;      // the stream: a debug broker and the box online
  t.view.hidden = !d.sn || d.state === "flashing";         // the status it sends all the time: any box
  t.cfg.hidden = !d.sn || !["online", "pass", "fail", "stopped", "shipped", "ship_fail"].includes(d.state);
  if (document.activeElement !== t.dbgin) t.dbgin.checked = !!d.debug;
  t.log.hidden = !d.mac;
  return t;
}
const hm = (s) => { s = Math.max(0, s | 0); const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60);
  return h >= 24 ? `${Math.floor(h / 24)} วัน ${h % 24} ชม.` : h ? `${h} ชม. ${m} นาที` : m ? `${m} นาที` : `${s} วินาที`; };

const bucket = (s) => ["usb_fail", "stopped", "ship_fail"].includes(s) ? "fail" : s === "shipping" ? "running" : s;
const pending = () => (devs ? devs.questions : []).filter((x) => !answered.has(x.id));
const firstQ = () => (pending()[0] || {}).id;
function render() {
  const d = devs;
  const rows = [...d.flashing.map((x) => ({ ...x, mac: x.mac, progress: 0 })), ...d.rows];
  const counts = {};
  for (const r of rows) { const b = bucket(r.state); counts[b] = (counts[b] || 0) + 1; }
  const qs = pending();
  counts.ask = new Set(qs.map((q) => q.sn)).size;
  counts.soak = rows.filter((r) => r.soak && r.soak.running).length;
  document.querySelectorAll("#kpis .kpi").forEach((k) => {
    text(k.querySelector("b"), String(counts[k.dataset.f] || 0));
    k.classList.toggle("sel", k.dataset.f === filter);
    if (k.dataset.f === "ask") k.classList.toggle("hot", qs.length > 0);
  });
  $("soakstop").hidden = !counts.soak;
  const link = $("link");
  link.classList.toggle("up", d.broker.connected);
  text(link.querySelector("span"), d.broker.connected ? "broker ต่อแล้ว" : "broker ไม่ได้ต่อ");
  link.title = d.broker.error || "";
  if (document.activeElement !== $("auto")) $("auto").checked = d.auto;
  const err = $("err"); const msg = !d.broker.connected && d.broker.error ? "MQTT: " + d.broker.error : "";
  err.hidden = !msg; text(err, msg);

  if (qs.length && qs[qs.length - 1].id > lastQ) { lastQ = qs[qs.length - 1].id; beep(); }
  const grid = $("grid");
  const asked = new Set(qs.map((q) => q.sn));
  const want = rows.filter((r) => !filter || (filter === "ask" ? asked.has(r.sn) : filter === "soak" ? r.soak && r.soak.running
                                                                                 : bucket(r.state) === filter));
  const keep = new Set(want.map((r) => r.sn));
  for (const [sn, t] of tiles) if (!keep.has(sn)) { t.el.remove(); if (!rows.some((r) => r.sn === sn)) tiles.delete(sn); }
  want.sort((a, b) => (a.unit ?? 1e9) - (b.unit ?? 1e9) || String(a.sn).localeCompare(b.sn));
  want.forEach((r, i) => { const t = tile(r); if (grid.children[i] !== t.el) grid.insertBefore(t.el, grid.children[i] || null); });
  $("empty").hidden = rows.length > 0;
  document.title = (qs.length ? `(${qs.length}) ` : "") + "mCOLD Line";
}

const answered = new Set();
function answer(q, v) { answered.add(q.id); if (devs) render(); post("/api/line/answer", { qid: q.id, value: v }, true); }
// Y / N / R: the oldest question waiting (marked in its tile)
document.addEventListener("keydown", (e) => {
  if (!devs || e.key === "Enter" || e.ctrlKey || e.metaKey || e.altKey) return;
  if (document.querySelector("dialog[open]") || ["INPUT", "SELECT", "TEXTAREA"].includes((document.activeElement || {}).tagName)) return;
  const q = pending()[0];
  if (!q) return;
  const k = e.key.toLowerCase();
  const v = k === "y" ? true : k === "n" ? false : (k === "r" && q.repeat) ? "again" : undefined;
  if (v === undefined) return;
  e.preventDefault(); answer(q, v);
});
document.querySelectorAll("#kpis .kpi").forEach((k) => {
  k.onclick = () => { filter = filter === k.dataset.f ? "" : k.dataset.f; store("line.filter", filter); render(); };
});

/* ---- retest: before shipping straight away; after, factory mode back on over the USB cable first ---- */
let facDev = null;
async function startRetest(sn) {
  beep();
  const j = await post("/api/retest/start", { sns: [sn], operator: op.value });
  if (!j.error) toast(`เริ่มทดสอบซ้ำ ${sn}`, true);
}
function retest(d) {
  if (!d.factory_off) { startRetest(d.sn); return; }
  facDev = d;
  text($("factitle"), `ทดสอบซ้ำ · เครื่อง ${pad(d.unit)} · ${d.sn}`);
  $("facerr").hidden = true; $("facgo").disabled = false;
  $("facdlg").showModal();
}
$("facgo").onclick = async () => {        // the click itself opens the port picker (Web Serial needs it)
  $("facgo").disabled = true; $("facerr").hidden = true;
  try {
    await factoryOn(cfg, facDev.sn);
    $("facdlg").close(); toast("เปิดโหมดโรงงานแล้ว · ถอดสายได้", true);
    await startRetest(facDev.sn);
  } catch (e) {
    if (!(e && e.name === "NotFoundError")) { const x = $("facerr"); x.textContent = String(e && e.message || e); x.hidden = false; }
  } finally { $("facgo").disabled = false; }
}
$("facskip").onclick = () => { $("facdlg").close(); startRetest(facDev.sn); };

/* ---- the long test ---- */
async function stopSoak(sns) {
  const n = sns.length || (devs ? devs.rows.filter((r) => r.soak && r.soak.running).length : 0);
  if (!confirm(`จบทดสอบนาน ${n} เครื่อง แล้วสรุปผล?`)) return;
  const j = await post("/api/soak/stop", { sns });
  if (j.error) return;
  poll();
  if (j.stopped.length === 1) openSoak(devs.rows.find((r) => r.sn === j.stopped[0].sn) || { sn: j.stopped[0].sn }, j.stopped[0]);
  else showSoakResults(j.stopped);
}
$("soakall").onclick = async () => {
  const j = await post("/api/soak/start", { sns: [], operator: op.value });
  if (!j.error) toast(j.started ? `เริ่มทดสอบนาน ${j.started} เครื่อง` : "ไม่มีเครื่อง online ที่ยังไม่ได้ทดสอบนาน", !!j.started);
};
$("soakstop").onclick = () => stopSoak([]);
function findingsTable(box, findings) {
  const tb = document.createElement("table"); tb.className = "findings";
  for (const f of findings) {
    const tr = document.createElement("tr");
    for (const [v, c] of [[f.result === "PASS" ? "ปกติ" : "ไม่ปกติ", f.result], [f.name, ""], [f.detail, "wrap"]]) {
      const td = document.createElement("td"); td.textContent = v; if (c) td.className = c; tr.appendChild(td);
    }
    tb.appendChild(tr);
  }
  box.appendChild(tb);
}
function showSoakResults(list) {
  soakSn = "";
  text($("soaktitle"), `สรุปทดสอบนาน · ${list.length} เครื่อง`);
  const box = $("soakbody"); box.innerHTML = "";
  for (const r of list) {
    const h = document.createElement("h4"); h.className = "sres";
    h.innerHTML = '<span class="pill"></span><span class="mono"></span>';
    h.firstChild.className = "pill " + r.result; h.firstChild.textContent = r.result === "PASS" ? "ปกติ" : "ไม่ปกติ";
    h.lastChild.textContent = r.sn; box.appendChild(h); findingsTable(box, r.findings);
  }
  $("soakcharts").innerHTML = ""; $("soakspan")?.remove();
  if (!$("soakdlg").open) $("soakdlg").showModal();
}
let soakSn = "", soakTimer = null;
const SERIES = [["temp", "อุณหภูมิ (°C)"], ["rssi", "สัญญาณ Wi-Fi (dBm)"], ["soc", "แบต (%)"], ["ma", "กระแสแบต (mA)"],
                ["internal", "RAM ภายใน (KB)", 1 / 1024], ["online", "online (1) / offline (0)"]];
async function openSoak(d, stopped) {
  soakSn = d.sn;
  text($("soaktitle"), `ทดสอบนาน · เครื่อง ${pad(d.unit)} · ${d.sn}`);
  const box = $("soakbody"); box.innerHTML = "";
  const running = d.soak && d.soak.running && !stopped;
  if (stopped) findingsTable(box, stopped.findings);
  else if (running) {
    const n = document.createElement("div"); n.className = "note";
    n.textContent = `กำลังทดสอบ ${hm(d.soak.elapsed)} · อ่านแล้ว ${d.soak.samples} ครั้ง · กด จบ ที่ช่องของเครื่องเพื่อสรุปผล`
      + (d.soak.problems.length ? " · ไม่ปกติ: " + d.soak.problems.join(" · ") : " · ยังปกติทุกข้อ");
    box.appendChild(n);
  } else {
    const runs = await get(`/api/devices/${encodeURIComponent(d.mac)}/log`);
    const r = runs.find((x) => x.stage === "soak" && x.result);
    if (r) findingsTable(box, r.steps.map((s) => ({ name: s.name, result: s.result, detail: s.detail })));
  }
  if (!$("soakdlg").open) $("soakdlg").showModal();
  drawSoak();
}
async function drawSoak() {
  clearTimeout(soakTimer);
  if (!soakSn || !$("soakdlg").open) return;
  const j = await get(`/api/soak/${encodeURIComponent(soakSn)}`);
  const host = $("soakcharts"); host.innerHTML = ""; $("soakspan")?.remove();
  const sm = j.samples || [];
  if (!sm.length) { host.innerHTML = '<div class="note">ยังไม่มีค่าที่อ่าน</div>'; }
  for (const [k, label, scale] of SERIES) {
    const pts = sm.map((s) => k === "online" ? (s.online ? 1 : 0) : typeof s[k] === "number" ? s[k] * (scale || 1) : null);
    const nums = pts.filter((v) => v != null);
    if (!nums.length) continue;
    const el = document.createElement("div"); el.className = "ch";
    el.innerHTML = `<h5><span></span><b></b></h5><canvas width="320" height="60"></canvas>`;
    el.querySelector("span").textContent = label;
    const lo = Math.min(...nums), hi = Math.max(...nums);
    el.querySelector("b").textContent = k === "online" ? `${Math.round(100 * nums.filter((v) => v).length / nums.length)} %`
      : `${+lo.toFixed(1)} – ${+hi.toFixed(1)}`;
    host.appendChild(el); spark(el.querySelector("canvas"), pts);
  }
  if (sm.length) {
    const n = document.createElement("div"); n.className = "note";
    n.textContent = `${new Date(sm[0].at * 1000).toLocaleString("th-TH")} – ${new Date(sm[sm.length - 1].at * 1000).toLocaleString("th-TH")} · ${sm.length} ครั้ง ทุก ${j.interval} วินาที`;
    n.id = "soakspan"; n.style.marginTop = "6px"; host.after(n);
  }
  soakTimer = setTimeout(drawSoak, 5000);
}
function spark(cv, pts) {
  const nums = pts.filter((v) => v != null); const g = cv.getContext("2d");
  const lo = Math.min(...nums), hi = Math.max(...nums), span = hi - lo || 1, n = Math.max(pts.length - 1, 1);
  g.clearRect(0, 0, cv.width, cv.height);
  g.strokeStyle = getComputedStyle(root).getPropertyValue("--accent"); g.lineWidth = 1.5; g.beginPath();
  let pen = false;
  pts.forEach((v, i) => {
    if (v == null) { pen = false; return; }
    const x = i * cv.width / n, y = cv.height - 3 - (v - lo) / span * (cv.height - 6);
    if (pen) g.lineTo(x, y); else g.moveTo(x, y); pen = true;
  });
  g.stroke();
}

/* ---- one box's settings (its config values, over MQTT) ---- */
let cfgDev = null, cfgCur = null;
async function openCfg(d) {
  cfgDev = d;
  text($("cfgtitle"), `ตั้งค่าเครื่อง ${pad(d.unit)} · ${d.sn}`);
  $("cfgerr").hidden = true; $("cfgres").hidden = true; $("cfgbody").innerHTML = ""; text($("cfgnote"), "กำลังอ่าน…");
  if (!$("cfgdlg").open) $("cfgdlg").showModal();
  const j = await post(`/api/devices/${encodeURIComponent(d.sn)}/config`, null, false, "GET");
  if (j.error) { text($("cfgnote"), String(j.error)); return; }
  cfgCur = j;
  const nb = $("cfgnote");
  nb.className = "banner " + (j.factory ? "info" : "warn");
  text(nb, (j.factory ? "โหมดโรงงาน: ส่งเป็นเอกสารของสายผลิต (factory) ไม่กระทบ rev ของ server"
                      : "ส่งมอบแล้ว: ส่งเป็นเอกสารตั้งค่าปกติ ขยับ server_rev ของกล่อง · ถ้า server ส่งเอกสารใหม่ทีหลัง ค่าจะถูกแทน")
       + (j.reported ? "" : " · กล่องยังไม่ส่งค่าปัจจุบันมา") + (j.online ? "" : " · กล่อง offline: ส่งไม่ได้"));
  const tb = $("cfgbody");
  for (const [k, r] of Object.entries(j.keys)) {
    const tr = document.createElement("tr");
    tr.innerHTML = "<td></td><td class='k'></td><td class='note'></td><td class='mono cur'></td><td><input inputmode='numeric'></td>";
    tr.children[0].textContent = r.label || k; tr.children[1].textContent = k;
    tr.children[2].textContent = r.min != null ? `${r.min}–${r.max}` : "";
    const v = j.config[k];
    tr.children[3].textContent = v ?? "—";
    const inp = tr.querySelector("input"); inp.dataset.key = k; inp.value = v ?? ""; inp.placeholder = r.default != null ? String(r.default) : "";
    inp.oninput = () => tr.classList.toggle("chg", inp.value.trim() !== String(v ?? ""));
    tb.appendChild(tr);
  }
  text($("cfgnet"), j.network.length ? "Wi-Fi ในกล่อง: " + j.network.join(", ") : "");
  $("cfgsave").disabled = !j.online;
}
$("cfgsave").onclick = async () => {
  const changes = {};
  document.querySelectorAll("#cfgbody input").forEach((i) => {
    const v = i.value.trim(); if (v !== "" && v !== String(cfgCur.config[i.dataset.key] ?? "")) changes[i.dataset.key] = v; });
  if (!Object.keys(changes).length) { toast("ไม่มีค่าที่เปลี่ยน"); return; }
  $("cfgsave").disabled = true; $("cfgerr").hidden = true; $("cfgres").hidden = true;
  const j = await post(`/api/devices/${encodeURIComponent(cfgDev.sn)}/config`, { config: changes, operator: op.value }, true);
  $("cfgsave").disabled = false;
  if (j.error) { const e = $("cfgerr"); e.textContent = String(j.error); e.hidden = false; return; }
  const res = $("cfgres"); res.hidden = false;
  const ok = j.state === "ok";
  res.className = "banner " + (ok ? "info" : "err");
  res.textContent = ok ? (j.applied && j.applied.length ? "กล่องรับค่าแล้ว: " + j.applied.join(", ") : j.note || "กล่องรับแล้ว")
                       : `กล่องไม่รับ (${j.state}): ${JSON.stringify(j.errors || j.why || "")}`;
  if (ok) openCfg(cfgDev).then(() => { $("cfgres").hidden = false; });
};

let pollTimer = null;
async function poll() {
  clearTimeout(pollTimer);
  try { devs = await get("/api/devices"); render(); $("err").hidden = !(devs.broker && devs.broker.error && !devs.broker.connected); }
  catch (e) { const err = $("err"); err.hidden = false; err.textContent = "ติดต่อ server ไม่ได้"; }
  pollTimer = setTimeout(poll, 1000);
}

/* ---- adding a device ---- */
const unitIn = $("unit");
const form = JSON.parse(load("line.addform") || "{}");
function saveForm() { store("line.addform", JSON.stringify(form)); }
function setUnit(u) { if (u != null) { unitIn.value = pad(u); updatePreview(); } }
function updatePreview() {
  const u = parseInt(unitIn.value, 10);
  text($("snprev"), cfg && cfg.lot && Number.isFinite(u) ? `SN: ${cfg.lot}-${pad(u)}` : "กรอกล็อตก่อน");
}
unitIn.oninput = () => { unitIn.value = unitIn.value.replace(/\D/g, "").slice(0, cfg ? cfg.unit_digits : 3); updatePreview(); };
async function stepUnit(dir) {
  const u = parseInt(unitIn.value, 10) || 0;
  if (dir > 0) { const j = await get(`/api/next_unit?after=${u}`); setUnit(j.unit); }
  else setUnit(Math.max(1, u - 1));
}
$("uup").onclick = () => stepUnit(1);
$("udn").onclick = () => stepUnit(-1);
function renderWifiChoices() {
  const box = $("wifis"), key = JSON.stringify(cfg.wifi);
  if (box.dataset.k === key) return;
  box.dataset.k = key; box.innerHTML = "";
  // the line's network and the customer's together: the box joins whichever it finds
  if (!form.wifi) form.wifi = cfg.wifi.slice(0, NET_MAX).map((w) => w.ssid);
  if (!cfg.wifi.length) box.innerHTML = '<div class="note">ยังไม่มี Wi-Fi ในหน้าตั้งค่า</div>';
  for (const w of cfg.wifi) {
    const l = document.createElement("label"); l.className = "chk";
    const c = document.createElement("input"); c.type = "checkbox"; c.checked = form.wifi.includes(w.ssid);
    c.onchange = () => {
      form.wifi = [...box.querySelectorAll("input")].map((x, i) => x.checked ? cfg.wifi[i].ssid : null).filter(Boolean);
      saveForm(); limitWifi();
    };
    l.append(c, document.createTextNode(" " + w.ssid + " "));
    const s = document.createElement("small"); s.textContent = w.label; l.appendChild(s);
    box.appendChild(l);
  }
  $("erase").checked = form.erase_all ?? cfg.erase_all;
  limitWifi();
}
function limitWifi() {
  const ins = [...$("wifis").querySelectorAll("input")], n = ins.filter((x) => x.checked).length;
  ins.forEach((x) => { x.disabled = !x.checked && n >= NET_MAX; });
  text($("wcount"), `(${n}/${NET_MAX})`);
}
$("erase").onchange = (e) => { form.erase_all = e.target.checked; saveForm(); };
function openAdd(open) {
  $("add").hidden = !open; store("line.addopen", open ? "1" : "");
  if (open) { if (!unitIn.value) setUnit(cfg.next_unit); updatePreview(); (cfg.lot ? $("addbtn") : lotIn).focus(); }
}
$("openadd").onclick = () => openAdd(true);
$("closeadd").onclick = () => openAdd(false);

const stepsEl = $("steps"), result = $("result");
function makeUi() {
  stepsEl.hidden = false; result.hidden = true; stepsEl.innerHTML = "";
  const li = {};
  for (const [k, label] of STEPS) {
    const e = document.createElement("li"); e.textContent = label; const em = document.createElement("em"); e.appendChild(em);
    li[k] = e; stepsEl.appendChild(e);
  }
  let cur = null;
  return {
    step(k, note) {
      if (cur && cur !== k) li[cur].className = "ok";
      cur = k; li[k].className = "now"; text(li[k].querySelector("em"), note || "");
    },
    progress(f) { if (cur) text(li[cur].querySelector("em"), Math.round(f * 100) + "%"); },
    log(s) { console.debug("[usb]", s); },
    prepared(p) { text($("snprev"), `SN: ${p.sn}${p.existing ? " (SN เดิมของกล่อง)" : ""}${p.factory_ip ? " · IP " + p.factory_ip : ""}`); },
    finish(ok) { if (cur) li[cur].className = ok ? "ok" : "bad"; },
  };
}
const recent = JSON.parse(load("line.recent") || "[]");
function renderRecent() {
  const r = $("recent"); r.innerHTML = "";
  for (const x of recent.slice(0, 5)) {
    const d = document.createElement("div");
    d.innerHTML = `<span>${x.ok ? "✓" : "✗"}</span><span class="mono"></span><span></span>`;
    d.children[1].textContent = x.sn || x.mac || ""; d.children[2].textContent = x.at;
    r.appendChild(d);
  }
}
renderRecent();
async function add() {
  if (busy || !cfg) return;
  if (!cfg.lot) { toast("กรอกล็อต SN ก่อน"); lotIn.focus(); return; }
  busy = true; $("addbtn").disabled = true;
  const ui = makeUi();
  const unit = parseInt(unitIn.value, 10);
  try {
    const rec = await addBox(cfg, { lot: cfg.lot, unit: Number.isFinite(unit) ? unit : null, wifi: form.wifi || [],
                                    erase_all: $("erase").checked, operator: op.value }, ui);
    ui.finish(true); beep();
    result.className = "result ok"; result.hidden = false;
    result.innerHTML = `<div>${icon("check")}</div><div><b></b><small>ถอดสาย วางกล่องบนที่ชาร์จ แล้วเสียบเครื่องถัดไป กด เพิ่ม</small></div>`;
    result.querySelector("b").textContent = `เครื่อง ${pad(cfg.lot ? parseInt(rec.sn.split("-").pop(), 10) : 0)} เสร็จ · ${rec.sn}`;
    recent.unshift({ ok: true, sn: rec.sn, at: new Date().toLocaleTimeString("th-TH") });
    setUnit(rec.next_unit);
  } catch (e) {
    ui.finish(false);
    const need = e instanceof NeedRom, cancelled = e && e.name === "NotFoundError";
    result.className = "result " + (need ? "ok" : "bad"); result.hidden = cancelled;
    result.innerHTML = `<div>${icon(need ? "info" : "x")}</div><div><b></b><small></small></div>`;
    result.querySelector("b").textContent = need ? "กด เพิ่ม อีกครั้ง" : "ไม่สำเร็จ";
    result.querySelector("small").textContent = String(e && e.message || e);
    if (cancelled) stepsEl.hidden = true;
    if (!need && !cancelled) recent.unshift({ ok: false, sn: "", mac: String(e.message || e).slice(0, 40), at: new Date().toLocaleTimeString("th-TH") });
  } finally {
    recent.splice(10); store("line.recent", JSON.stringify(recent)); renderRecent();
    busy = false; $("addbtn").disabled = false; $("addbtn").focus(); poll();
  }
}
$("addbtn").onclick = add;
document.addEventListener("keydown", (e) => {
  if (e.key !== "Enter" || $("add").hidden || document.querySelector("dialog[open]")) return;
  const a = document.activeElement;
  if (a && a !== unitIn && a !== $("addbtn") && ["INPUT", "SELECT", "TEXTAREA"].includes(a.tagName)) return;
  e.preventDefault(); add();
});

/* ---- a device's history ---- */
let logMac = "";
async function openLog(d) {
  logMac = d.mac;
  text($("logtitle"), `ประวัติ · ${d.sn || ""} · ${d.mac}`);
  const runs = await get(`/api/devices/${encodeURIComponent(d.mac)}/log`);
  const box = $("runs"); box.innerHTML = "";
  for (const r of runs) {
    const el = document.createElement("div"); el.className = "run";
    el.innerHTML = `<h4><span class="pill"></span><span></span><small></small></h4><table><tbody></tbody></table>`;
    el.querySelector(".pill").className = "pill " + r.result; el.querySelector(".pill").textContent = r.result;
    el.querySelector("h4 span:nth-child(2)").textContent = r.stage_label;
    el.querySelector("small").textContent = `${(r.started || "").replace("T", " ").slice(0, 19)} · ${r.firmware || ""} · ${r.operator || ""}`;
    const tb = el.querySelector("tbody");
    for (const s of r.steps) {
      const tr = document.createElement("tr");
      for (const [v, cls] of [[s.result, s.result], [s.name, ""], [s.detail || "", "wrap"]]) {
        const td = document.createElement("td"); td.textContent = v; if (cls) td.className = cls; tr.appendChild(td);
      }
      tb.appendChild(tr);
    }
    box.appendChild(el);
  }
  const del = document.createElement("button"); del.className = "btn sm"; del.innerHTML = icon("trash") + "ลบบันทึกเครื่องนี้";
  del.onclick = () => confirmDelete(d); box.appendChild(del);
  if (!$("logdlg").open) $("logdlg").showModal();
}
let confDev = null;
function confirmDelete(d) { confDev = d; text($("conf-what"), `${d.sn || "ยังไม่มี SN"} · ${d.mac}`); $("confdlg").showModal(); $("conf-no").focus(); }
$("conf-no").onclick = () => $("confdlg").close();
$("conf-yes").onclick = async () => {
  const j = await post(`/api/devices/${encodeURIComponent(confDev.mac)}?by=${encodeURIComponent(op.value)}`, null, false, "DELETE");
  $("confdlg").close();
  if (!j.error) { toast(`ลบ ${j.deleted.sn || j.deleted.mac} แล้ว`, true); $("logdlg").close(); tiles.get(confDev.sn)?.el.remove(); tiles.delete(confDev.sn); poll(); }
};

/* ---- debug: the status sent all the time, GET_DEBUG on request, the stream when on ---- */
let dbgWs = null, dbgDev = null;
const chans = {};
const kb = (n) => n == null ? "—" : n >= 1048576 ? (n / 1048576).toFixed(1) + " MB" : n >= 1024 ? (n / 1024).toFixed(1) + " KB" : n + " B";
const dur = (s) => { if (s == null) return "—"; s = Math.floor(s); const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  return (d ? d + " วัน " : "") + `${h}:${String(m).padStart(2, "0")}:${String(s % 60).padStart(2, "0")}`; };
const rssi = (v) => v == null ? ["—", ""] : [`${v} dBm · ${v >= -60 ? "ดีมาก" : v >= -70 ? "ดี" : v >= -80 ? "พอใช้" : "อ่อน"}`,
  v >= -70 ? "good" : v >= -80 ? "warn" : "bad"];
const yes = (v, on = "เปิด", off = "ปิด") => v == null ? "—" : v ? on : off;
// what each stream channel's fields mean (firmware dev.66, notes/firmware.md 13:40 / 17:10):
// a value the box could not read comes as null or is left out -> "—"
const num = (v, f) => typeof v === "number" ? f(v) : "—";
const signed = (v, dp) => num(v, (x) => `${x > 0 ? "+" : ""}${x.toFixed(dp)}`);
const RAM_LOW = 10240;     // internal RAM under ~10 KB: trouble (13:40)
const CH = {
  power: { title: "ไฟ / แบต", graph: ["ma", "กระแสแบต (mA, + = เข้าแบต)"], fields: (d, r) => [
    ["กระแสแบต", typeof r.ma === "number" ? signed(r.ma, 1) + " mA" : "อ่านไม่ได้", typeof r.ma !== "number" ? "warn" : r.ma > 0 ? "good" : ""],
    ["แรงดัน", num(d.mv, (v) => (v / 1000).toFixed(3) + " V")], ["แบต", num(d.soc, (v) => v + " %")],
    ["ชาร์จ", d.charge ?? "—"], ["ไฟเข้า (PG)", yes(d.pg, "มี", "ไม่มี")],
    ["fault", d.fault == null ? "—" : d.fault === 0 ? "ไม่มี" : "0x" + Number(d.fault).toString(16).padStart(2, "0"), d.fault ? "bad" : d.fault === 0 ? "good" : ""]] },
  sys: { title: "ระบบ / หน่วยความจำ", graph: ["internal", "RAM ภายใน (KB)"], fields: (d, r) => [
    ["RAM ภายใน", kb(r.internal), r.internal == null ? "" : r.internal < RAM_LOW ? "bad" : r.internal < 2 * RAM_LOW ? "warn" : "good"],
    ["heap", kb(r.heap)], ["heap ต่ำสุด", kb(r.min_heap)], ["PSRAM ว่าง", kb(r.psram)], ["เปิดเครื่องมา", dur(r.up_s)],
    ["โหมดโรงงาน", yes(r.factory)]] },
  net: { title: "เครือข่าย", graph: ["rssi", "สัญญาณ (dBm)"], fields: (d, r) => [
    ["Wi-Fi", yes(r.wifi, "ต่อแล้ว", "ไม่ได้ต่อ"), r.wifi ? "good" : r.wifi === false ? "bad" : ""],
    ["สัญญาณ", ...(r.wifi === false ? ["—", ""] : rssi(r.rssi))],
    ["broker หลัก", yes(r.broker1, "ต่อแล้ว", "ไม่ได้ต่อ"), r.broker1 ? "good" : r.broker1 === false ? "bad" : ""]] },
};
function kvs(box, list) {
  box.innerHTML = "";
  for (const [k, v, cls] of list) {
    const d = document.createElement("div"); d.className = "kv";
    d.innerHTML = "<span></span><b></b>"; d.firstChild.textContent = k; d.lastChild.textContent = v ?? "—";
    if (cls) d.lastChild.className = cls; box.appendChild(d);
  }
}
function renderBase(m) {
  const s = m.status || {}, t = s.temp || {}, p = s.power || {}, n = s.net || {}, fw = s.fw || {}, tr = s.trip || {}, g = s.gnss || {};
  if (!m.status_at) { $("dbgbase").innerHTML = '<div class="note">ยังไม่มี status จากกล่องนี้ (offline?)</div>'; return; }
  kvs($("dbgbase"), [
    ["สถานะ", m.online ? "online" : "offline", m.online ? "good" : "bad"],
    ["อุณหภูมิ", t.ok ? `${t.c} °C` : "อ่านไม่ได้", t.ok ? "" : "bad"],
    ["แบต", [num(p.soc, (v) => v + " %"), num(p.mv, (v) => (v / 1000).toFixed(2) + " V")].filter((x) => x !== "—").join(" · ") || "—"],
    ["กระแส", num(p.ma, (v) => signed(v, Number.isInteger(v) ? 0 : 1) + " mA")], ["ชาร์จ", p.charge ?? "—"],
    ["ไฟเลี้ยงภายนอก", yes(p.external, "มี", "ไม่มี")], ["Wi-Fi", n.ssid || (n.connected ? "?" : "ไม่ได้ต่อ")],
    ["สัญญาณ", ...rssi(n.rssi)], ["IP", n.ip || "—"], ["firmware", `${fw.ver || "—"}${fw.slot ? " · " + fw.slot : ""}`],
    ["trip", tr.active ? `กำลังทำ #${tr.id}` : "ไม่มี", tr.active ? "warn" : ""], ["GNSS", g.fix ? "fix" : "ไม่มี fix"],
    ["พื้นที่ log", s.storage ? `${s.storage.used_pct} %` : "—"]]);
  dbgBaseAt = m.status_at;
}
let dbgBaseAt = 0;
setInterval(() => { if ($("dbgdlg").open && dbgBaseAt) text($("dbgage"), `อัปเดต ${Math.round(Date.now() / 1000 - dbgBaseAt)} วินาทีที่แล้ว`); }, 1000);
$("dbgget").onclick = async () => {
  const r = await post(`/api/devices/${encodeURIComponent(dbgDev.sn)}/get_debug`);
  if (r.error) return;
  const box = $("dbginfo"); box.hidden = false;
  kvs(box, [["GET_DEBUG", new Date().toLocaleTimeString("th-TH")], ["reset ล่าสุด", r.reset ?? "—"], ["boot ครั้งที่", r.boot ?? "—"],
    ["เปิดเครื่องมา", dur(r.uptime_s)], ["core dump", r.coredump ? "มี" : "ไม่มี", r.coredump ? "bad" : "good"],
    ["RAM ภายใน", kb(r.internal), r.internal < RAM_LOW ? "bad" : ""], ["RAM ภายในต่ำสุด", kb(r.min_internal)],
    ["ก้อนใหญ่สุด", kb(r.largest_internal)], ["heap", kb(r.heap)], ["heap ต่ำสุด", kb(r.min_heap)], ["PSRAM ว่าง", kb(r.psram)],
    ["โหมดโรงงาน", yes(r.factory_mode)], ["firmware", r.fw ?? "—"], ["ELF", r.elf ?? "—"]]);
};
function openDebug(d) {
  dbgDev = d; dbgBaseAt = 0;
  text($("dbgtitle"), `Debug · เครื่อง ${pad(d.unit)} · ${d.sn}`);
  $("dbgon").checked = !!d.debug;
  $("dbgch").innerHTML = ""; $("dbglog").textContent = ""; $("dbglog").hidden = true; $("dbginfo").hidden = true;
  text($("dbgage"), ""); $("dbgbase").innerHTML = "";
  for (const k of Object.keys(chans)) delete chans[k];
  if (dbgWs) dbgWs.close();
  const proto = location.protocol === "https:" ? "wss" : "ws";
  dbgWs = new WebSocket(`${proto}://${location.host}/api/devices/${encodeURIComponent(d.sn)}/debug/ws`);
  dbgWs.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    $("dbgon").checked = m.on; renderBase(m);
    for (const x of m.messages) onDebug(x);
  };
  if (!$("dbgdlg").open) $("dbgdlg").showModal();
}
$("dbgdlg").addEventListener("close", () => { if (dbgWs) dbgWs.close(); dbgWs = null; });
$("dbgon").onchange = async (e) => {
  const j = await post(`/api/devices/${encodeURIComponent(dbgDev.sn)}/debug`, { on: e.target.checked, minutes: 60 });
  if (j.error) e.target.checked = !e.target.checked;
};
function onDebug(m) {
  if (m.ch === "log") {
    const l = $("dbglog"), at = new Date(m.at * 1000).toLocaleTimeString("th-TH");
    l.hidden = false;
    for (const line of (m.data.lines || [JSON.stringify(m.data)])) l.textContent += `${at} ${line}\n`;
    if (m.data.dropped) l.textContent += `${at} … ทิ้งไป ${m.data.dropped} ไบต์ (buffer เต็ม)\n`;
    if (l.textContent.length > 20000) l.textContent = l.textContent.slice(-15000);
    l.scrollTop = l.scrollHeight; return;
  }
  const rows = (m.data && m.data.rows) || [];
  if (!rows.length) return;
  const spec = CH[m.ch];
  let c = chans[m.ch];
  if (!c) {
    const el = document.createElement("div"); el.className = "ch";
    el.innerHTML = `<h5><span></span></h5><div class="kvs"></div><div class="gl"></div><canvas width="300" height="46"></canvas>`;
    el.querySelector("h5 span").textContent = spec ? spec.title : m.ch;
    $("dbgch").appendChild(el);
    const nums = Object.keys(rows[0]).filter((k) => typeof rows[0][k] === "number");
    const key = spec ? spec.graph[0] : nums[0];      // a first row may lack it (unread)
    c = chans[m.ch] = { el, pts: [], key };
    text(el.querySelector(".gl"), spec ? spec.graph[1] : key || "");
  }
  for (const r of rows) if (c.key) { const v = r[c.key]; c.pts.push(typeof v !== "number" ? null : c.key === "internal" ? v / 1024 : v); }
  c.pts.splice(0, Math.max(0, c.pts.length - 300));
  const last = rows[rows.length - 1];
  const list = spec ? spec.fields(m.data, last)
    : Object.entries(last).map(([k, v]) => [k, typeof v === "object" ? JSON.stringify(v) : String(v)]);
  const box = c.el.querySelector(".kvs"); box.innerHTML = "";
  for (const [k, v, cls] of list) {
    const a = document.createElement("span"); a.textContent = k;
    const b = document.createElement("b"); b.textContent = v ?? "—"; if (cls) b.className = cls;
    box.append(a, b);
  }
  if (c.pts.some((v) => v != null)) spark(c.el.querySelector("canvas"), c.pts);
}

/* ---- SN summary ---- */
$("summary").onclick = async () => {
  const sm = await get("/api/summary?lot=" + encodeURIComponent(cfg.lot));
  if (sm.error) { toast(sm.error); return; }
  $("sumcsv").href = "/api/summary.csv?lot=" + encodeURIComponent(cfg.lot);
  text($("sumtitle"), "สรุป SN · " + (sm.lot ? "ล็อต " + sm.lot : "ทุกล็อต"));
  const bar = $("sumbar"); bar.innerHTML = "";
  for (const [l, v, c] of [["ผ่าน", sm.counts.PASS, "k-pass"], ["ไม่ผ่าน", sm.counts.FAIL, "k-fail"], ["ยังไม่ครบ", sm.counts.INCOMPLETE, "k-ask"]]) {
    const k = document.createElement("div"); k.className = "kpi " + c; k.innerHTML = "<span></span><b></b>";
    k.firstChild.textContent = l; k.lastChild.textContent = v; bar.appendChild(k);
  }
  const tb = $("sumbody"); tb.innerHTML = "";
  for (const r of sm.rows) {
    const tr = document.createElement("tr");
    const add = (v, cls) => { const td = document.createElement("td"); if (v instanceof Node) td.appendChild(v); else td.textContent = v; if (cls) td.className = cls; tr.appendChild(td); };
    add(r.sn || "—", "mono");
    const p = document.createElement("span"); p.className = "pill " + r.result;
    p.textContent = { PASS: "ผ่าน", FAIL: "ไม่ผ่าน", INCOMPLETE: "ยังไม่ครบ" }[r.result]; add(p);
    add(r.mac, "mono"); add(r.firmware || "—", "mono");
    add(r.fail ? `${r.fail.stage_label}: ${r.fail.step}` : "", "wrap");
    add((r.finished || "").replace("T", " ").slice(0, 16)); add(r.operator || "");
    const del = document.createElement("button"); del.className = "btn sm icon"; del.innerHTML = icon("trash");
    del.onclick = () => confirmDelete({ sn: r.sn, mac: r.mac }); add(del);
    tb.appendChild(tr);
  }
  $("sumdlg").showModal();
};

/* ---- firmware ---- */
async function renderLibrary() {
  const rows = await get("/api/firmware/library"), tb = $("fwlib"); tb.innerHTML = "";
  for (const m of rows) {
    const tr = document.createElement("tr");
    for (const v of [m.version, m.id, m.built, m.signed ? "เซ็นแล้ว" : "ไม่ได้เซ็น", m.approved ? "✓ " + m.by : "ยังไม่อนุมัติ"]) {
      const td = document.createElement("td"); td.textContent = v; tr.appendChild(td);
    }
    const td = document.createElement("td");
    if (!m.approved) {
      const b = document.createElement("button"); b.className = "btn sm"; b.textContent = "อนุมัติ";
      b.onclick = async () => {
        const by = $("fwby").value.trim() || op.value.trim();
        if (!by) { toast("ใส่ชื่อผู้อนุมัติ (ช่อง อนุมัติโดย หรือ ผู้ทำ)"); return; }
        const j = await post("/api/firmware/approve", { id: m.id, by });
        if (!j.error) { await loadConfig(); renderLibrary(); fillFirmware(); }
      };
      td.appendChild(b);
    }
    tr.appendChild(td); tb.appendChild(tr);
  }
}
$("fwup").onclick = async () => {
  const f = $("fwzip").files[0];
  if (!f) { toast("เลือกไฟล์ zip ก่อน"); return; }
  const r = await fetch("/api/firmware/import?approve_by=" + encodeURIComponent($("fwby").value.trim()),
                        { method: "POST", headers: { "Content-Type": "application/zip" }, body: f });
  const j = await r.json();
  if (!r.ok) { toast(j.error || r.status); return; }
  toast(`นำเข้า ${j.version}${j.approved ? " และอนุมัติแล้ว" : " (ยังไม่อนุมัติ)"}`, true);
  $("fwzip").value = ""; await loadConfig(); renderLibrary(); fillFirmware();
};
$("fwbtn").onclick = () => { fillFirmware(); renderLibrary(); $("fwdlg").showModal(); };
function fillFirmware() {
  for (const [id, k] of [["fwusb", "usb"], ["fwfinal", "final"]]) {
    const sel = $(id); sel.innerHTML = "";
    const a = document.createElement("option"); a.value = "approved";
    a.textContent = cfg.firmware_options.length ? `ล่าสุด · ${cfg.firmware_options[0].version}` : "ยังไม่มีที่อนุมัติ"; sel.appendChild(a);
    for (const o of cfg.firmware_options) {
      const e = document.createElement("option"); e.value = o.id; e.textContent = `${o.version} · ${o.by}${o.signed ? "" : " · ไม่ได้เซ็น"}`; sel.appendChild(e);
    }
    sel.value = cfg.firmware[k].which || "approved";
    sel.onchange = async () => { const j = await post("/api/firmware", { [k]: sel.value }); if (!j.error) { await loadConfig(); toast("บันทึกแล้ว", true); } };
  }
}

/* ---- history ---- */
$("histbtn").onclick = async () => {
  const rows = await get("/api/history"); const tb = $("histbody"); tb.innerHTML = "";
  for (const r of rows) {
    const tr = document.createElement("tr");
    for (const [v, c] of [["#" + r.id], [(r.started || "").replace("T", " ").slice(0, 19)], [r.stage], [r.result || "…", r.result || ""],
                          [r.sn || "", "mono"], [r.mac || "", "mono"], [r.firmware || ""], [r.operator || ""]]) {
      const td = document.createElement("td"); td.textContent = v; if (c) td.className = c; tr.appendChild(td);
    }
    tb.appendChild(tr);
  }
  $("histdlg").showModal();
};

/* ---- settings ---- */
function wifiRow(w) {
  const row = document.createElement("div"); row.className = "wifirow";
  row.dataset.old = w.ssid || ""; row.dataset.has = w.has_password ? "1" : "";
  row.innerHTML = `<div class="frow">
      <label class="fld" style="flex:1 1 160px"><span>SSID</span><input class="w-ssid mono" maxlength="32"></label>
      <label class="fld" style="flex:1 1 160px"><span>รหัสผ่าน</span><input class="w-pass" type="password" autocomplete="new-password"></label>
      <label class="fld" style="width:120px"><span>IP</span><select class="w-dhcp"><option value="1">DHCP</option><option value="0">IP คงที่</option></select></label>
      <button class="btn sm icon w-del" title="ลบ">${icon("trash")}</button></div>
    <div class="frow w-fixed" style="margin-top:8px">
      <label class="fld" style="width:140px"><span>IP</span><input class="w-ip mono"></label>
      <label class="fld" style="width:140px"><span>Gateway</span><input class="w-gw mono"></label>
      <label class="fld" style="width:140px"><span>Subnet</span><input class="w-sub mono"></label>
      <label class="fld" style="width:140px"><span>DNS</span><input class="w-dns mono"></label></div>`;
  const q = (c) => row.querySelector(c);
  q(".w-ssid").value = w.ssid || ""; q(".w-pass").placeholder = w.has_password ? "•••• (ว่าง = ใช้เดิม)" : "8–63 ตัว";
  q(".w-dhcp").value = w.dhcp === false ? "0" : "1";
  q(".w-ip").value = w.ip || ""; q(".w-gw").value = w.gateway || ""; q(".w-sub").value = w.subnet || ""; q(".w-dns").value = w.dns || "";
  const sync = () => { q(".w-fixed").hidden = q(".w-dhcp").value === "1"; };
  q(".w-dhcp").onchange = sync; sync();
  q(".w-del").onclick = () => { row.remove(); countWifi(); };
  return row;
}
function countWifi() { const n = document.querySelectorAll("#f-wifi .wifirow").length; text($("f-wcount"), `(${n}/5)`); $("f-addwifi").disabled = n >= 5; }
$("f-addwifi").onclick = () => { $("f-wifi").appendChild(wifiRow({ dhcp: true })); countWifi(); };
const pw = (id, has) => { const e = $(id); e.value = ""; e.placeholder = has ? "•••• ตั้งไว้แล้ว (ว่าง = ใช้เดิม)" : ""; e.dataset.has = has ? "1" : ""; };
const pwval = (id) => $(id).value || ($(id).dataset.has ? null : "");
$("settings").onclick = async () => {
  const d = await get("/api/settings");
  $("f-station").value = d.station || "";
  const f = d.factory || {};
  $("f-fssid").value = f.ssid || ""; pw("f-fpass", f.has_password);
  for (const [id, k] of [["f-fbase", "ip_base"], ["f-fgw", "gateway"], ["f-fsub", "subnet"], ["f-fdns", "dns"]]) $(id).value = f[k] || "";
  text($("f-fnote"), f.ip_base ? `เครื่อง ${pad(1)} = ${f.first} · มีที่ให้ ${f.capacity} เครื่อง · ถูกลบออกจากกล่องตอนส่งมอบ` : "ไม่ใส่ IP ฐาน = DHCP");
  const wl = $("f-wifi"); wl.innerHTML = ""; for (const w of d.wifi) if (w.ssid !== f.ssid) wl.appendChild(wifiRow(w)); countWifi();
  $("f-mhost").value = d.mqtt.host || ""; $("f-mport").value = d.mqtt.port || 1883; $("f-muser").value = d.mqtt.username || ""; pw("f-mpass", d.mqtt.has_password);
  $("f-suser").value = (d.station_mqtt || {}).username || ""; pw("f-spass", (d.station_mqtt || {}).has_password);
  const dm = d.debug_mqtt || {};
  $("f-dhost").value = dm.host || ""; $("f-dport").value = dm.port || 1883; $("f-duser").value = dm.username || ""; pw("f-dpass", dm.has_password);
  $("f-ota").value = d.ota_base || "";
  const tb = $("f-cfg"); tb.innerHTML = "";
  for (const [k, r] of Object.entries(d.config_keys || {})) {
    const tr = document.createElement("tr");
    tr.innerHTML = "<td></td><td class='k'></td><td class='note'></td><td><input inputmode='numeric'></td>";
    tr.children[0].textContent = r.label || k; tr.children[1].textContent = k; tr.children[2].textContent = `${r.min}–${r.max} · เดิม ${r.default}`;
    const inp = tr.querySelector("input"); inp.dataset.key = k; inp.value = d.config[k] ?? ""; inp.placeholder = String(r.default);
    tb.appendChild(tr);
  }
  text($("f-path"), d.path || "site.ini"); $("f-err").hidden = true;
  $("setdlg").showModal();
};
$("setsave").onclick = async () => {
  const wifi = [...document.querySelectorAll("#f-wifi .wifirow")].map((row) => {
    const q = (c) => row.querySelector(c), typed = q(".w-pass").value;
    return { ssid: q(".w-ssid").value.trim(), old_ssid: row.dataset.old, dhcp: q(".w-dhcp").value === "1",
             password: typed || (row.dataset.has ? null : ""), ip: q(".w-ip").value.trim(), gateway: q(".w-gw").value.trim(),
             subnet: q(".w-sub").value.trim(), dns: q(".w-dns").value.trim() };
  });
  const config = {}; document.querySelectorAll("#f-cfg input").forEach((i) => { config[i.dataset.key] = i.value.trim(); });
  const body = { station: $("f-station").value.trim(), wifi, config, ota_base: $("f-ota").value.trim(),
    mqtt: { host: $("f-mhost").value.trim(), port: parseInt($("f-mport").value, 10) || 1883, username: $("f-muser").value.trim(), password: pwval("f-mpass") },
    station_mqtt: { username: $("f-suser").value.trim(), password: $("f-spass").value || null },
    debug_mqtt: { host: $("f-dhost").value.trim(), port: parseInt($("f-dport").value, 10) || 1883, username: $("f-duser").value.trim(), password: pwval("f-dpass") },
    factory: { ssid: $("f-fssid").value.trim(), password: pwval("f-fpass"), ip_base: $("f-fbase").value.trim(),
               gateway: $("f-fgw").value.trim(), subnet: $("f-fsub").value.trim(), dns: $("f-fdns").value.trim() } };
  const j = await post("/api/settings", body, true);
  if (j.error) { const e = $("f-err"); e.textContent = j.error; e.hidden = false; e.scrollIntoView({ block: "nearest" }); return; }
  $("setdlg").close(); toast("บันทึกแล้ว ใช้กับเครื่องที่เพิ่มต่อจากนี้", true); loadConfig();
};

/* ---- start ---- */
(async () => {
  await loadConfig();
  if (load("line.addopen") || !cfg.lot) openAdd(true); else updatePreview();
  setUnit(cfg.next_unit);
  poll();
  setInterval(loadConfig, 15000);
})();
