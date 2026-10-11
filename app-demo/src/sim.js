// Simulated box + server. No Web Bluetooth, Web NFC or network calls: every "command" below is a
// local function with a delay, so the UI can show loading, progress, success and error states.
// Command names in comments map to docs/app/ble-protocol.md; they never appear in user-facing text.
import { getState, update, simNow, nextReqId, uuid, tripDate, toast, TRIP_LOCKED, stamp } from './store.js';
import { csvName } from './format.js';

const MIN = 60000;
const HOUR = 60 * MIN;
export const BATT_TRIP_MV = 3550; // batt_trip_mv: below this the box refuses a trip (device-settings.md)
export const CHUNK_ROWS = 16; // READ_LOG_CHUNK max

export class BleLost extends Error {
  constructor(when = 'during') {
    super('BLE_LOST');
    this.when = when;
  }
}

const wait = (ms) => new Promise((r) => setTimeout(r, ms));

export const findDevice = (s, sn) => s.devices.find((d) => d.sn === sn);
export const isLinked = (s, sn) => !!(s.ble && s.ble.sn === sn && s.ble.authorized);

// ---------------------------------------------------------------------------
// Deterministic temperature model
// ---------------------------------------------------------------------------
function hash(n) {
  let x = Math.floor(n) ^ 0x5bd1e995;
  x = Math.imul(x ^ (x >>> 15), 0x2c1b3c6d);
  x = Math.imul(x ^ (x >>> 12), 0x297a2d39);
  x ^= x >>> 15;
  return ((x >>> 0) % 10000) / 10000;
}

function episodeAt(dev, t) {
  let hit = null;
  for (const e of dev.episodes) if (e.from <= t && (e.to == null || t < e.to)) hit = e;
  return hit;
}

export function tripActiveAt(dev, t) {
  return dev.trips.find((tr) => tr.startUtc <= t && (tr.stopUtc == null || t <= tr.stopUtc)) || null;
}

/** Temperature the box would read at time t, after user adjustment. null = probe gave no value. */
export function tempAt(dev, t) {
  const ep = episodeAt(dev, t);
  if (ep?.type === 'probe') return null;
  const n = hash(t / MIN + dev.sn.length * 97 + dev.sn.charCodeAt(dev.sn.length - 1));
  if (ep?.type === 'high') {
    const trip = tripActiveAt(dev, t);
    return round2((trip ? trip.high : 8) + 2.1 + n * 0.6);
  }
  const wave = 0.7 * Math.sin((2 * Math.PI * t) / (5 * HOUR) + dev.baseTemp);
  return round2(dev.baseTemp + wave + (n - 0.5) * 0.5 + dev.config.temp_adj_c100 / 100);
}
const round2 = (v) => Math.round(v * 100) / 100;

/** Rows the box has logged for a trip up to `until` (samples + event rows). */
export function tripSeries(dev, trip, until) {
  const end = Math.min(trip.stopUtc ?? until, until);
  const period = trip.period * 1000;
  const n = end >= trip.startUtc ? Math.floor((end - trip.startUtc) / period) + 1 : 0;
  const step = Math.max(1, Math.ceil(n / 240)); // keep the chart light for long trips
  const points = [];
  let min = null;
  let max = null;
  for (let i = 0; i < n; i++) {
    const t = trip.startUtc + i * period;
    const c = tempAt(dev, t);
    if (c != null) {
      min = min == null ? c : Math.min(min, c);
      max = max == null ? c : Math.max(max, c);
    }
    if (i % step === 0 || i === n - 1) points.push({ t, c });
  }
  const events = trip.events.filter((e) => e.t <= end);
  for (const e of events) {
    if (typeof e.temp === 'number') {
      min = min == null ? e.temp : Math.min(min, e.temp);
      max = max == null ? e.temp : Math.max(max, e.temp);
    }
  }
  return { points, samples: n, rows: n + events.length, min, max, events, end };
}

const ALARM_TYPES = ['ALARM_HIGH', 'ALARM_LOW', 'ALARM_PROBE', 'BATTERY_LOW'];

/** Replay trip events up to `until`: which alarms are still active, and was the latest one acknowledged. */
export function alarmState(trip, until) {
  const active = new Set();
  let acked = false;
  let raised = 0;
  let last = null;
  for (const e of trip.events) {
    if (e.t > until) break;
    if (ALARM_TYPES.includes(e.type)) {
      active.add(e.type);
      acked = false;
      raised++;
      last = e;
    } else if (e.type === 'ALARM_CLEAR') {
      active.delete(e.detail);
    } else if (e.type === 'ALARM_ACK') {
      acked = true;
    }
  }
  return { active: [...active], acked, raised, last };
}

/**
 * What the app knows about a box. Connected: live values. Otherwise: the snapshot from the last
 * time the app talked to it (`live: false`), or nothing at all (`known: false`).
 */
export function deviceView(s, dev) {
  const now = simNow();
  const live = isLinked(s, dev.sn);
  const at = live ? now : dev.lastSyncAt;
  if (at == null) {
    return { known: false, live: false, at: null, temp: null, battery: null, batteryMv: null, storagePct: null, trip: null, lastTrip: null, gnss: { state: 'unknown' }, probeOk: null };
  }
  const temp = tempAt(dev, at);
  const tripObj = dev.activeTripId ? dev.trips.find((t) => t.trip_id === dev.activeTripId) : null;
  // A start whose answer was lost: until the app talks to the box again it cannot know the trip began.
  const unconfirmed = !live && s.pendingCmd[dev.sn]?.kind === 'START';
  const activeTrip = tripObj && tripObj.startUtc <= at && !unconfirmed ? tripObj : null;
  let trip = null;
  if (activeTrip) {
    const ser = tripSeries(dev, activeTrip, at);
    trip = { ...activeTrip, ...ser, alarms: alarmState(activeTrip, at) };
  }
  const g = dev.gnss;
  let gnss = { state: 'none' };
  if (g.fixAt != null && g.fixAt <= at) {
    const fresh = at - g.fixAt <= dev.config.gnss_period_s * 1000 + 10 * MIN;
    gnss = { state: fresh ? 'fix' : 'last', lat: g.lat, lon: g.lon, fixAt: g.fixAt };
  }
  const finished = dev.trips.filter((t) => t.stopUtc != null && t.stopUtc <= at);
  return {
    known: true,
    live,
    at,
    temp,
    probeOk: temp != null,
    battery: dev.battery,
    batteryMv: dev.batteryMv,
    storagePct: dev.storagePct,
    trip,
    lastTrip: finished[finished.length - 1] || null,
    gnss,
    timeQuality: dev.timeQuality,
    external: false,
    wifiNow: dev.wifiNow,
  };
}

export function inRange(c, trip) {
  if (c == null || !trip) return null;
  return c >= trip.low && c <= trip.high;
}

// ---------------------------------------------------------------------------
// Trip delivery status (box flag + app queue)
// ---------------------------------------------------------------------------
export const STEP_KEYS = ['ble', 'csv', 'upload', 'server', 'device'];

export function transferSteps(tr) {
  const st = tr?.stage || 'queued';
  const S = {};
  const set = (k, v) => (S[k] = v);
  STEP_KEYS.forEach((k) => set(k, 'todo'));
  const done = (...ks) => ks.forEach((k) => set(k, 'done'));
  switch (st) {
    case 'receiving': set('ble', 'active'); break;
    case 'ble_interrupted': set('ble', 'error'); break;
    case 'received': done('ble'); break;
    case 'csv': done('ble'); set('csv', 'active'); break;
    case 'ready_upload': done('ble', 'csv'); break;
    case 'waiting_internet': done('ble', 'csv'); set('upload', 'wait'); break;
    case 'uploading': done('ble', 'csv'); set('upload', 'active'); break;
    case 'awaiting_server': done('ble', 'csv', 'upload'); set('server', 'active'); break;
    case 'upload_failed': done('ble', 'csv'); set('upload', 'error'); break;
    case 'server_ok': done('ble', 'csv', 'upload', 'server'); break;
    case 'marking': done('ble', 'csv', 'upload', 'server'); set('device', 'active'); break;
    case 'mark_failed': done('ble', 'csv', 'upload', 'server'); set('device', 'wait'); break;
    case 'done': done(...STEP_KEYS); break;
    default: break;
  }
  return S;
}

/** One status per finished trip, combining the box's `sent` flag and the app's queue. */
export function tripDelivery(s, dev, trip) {
  if (trip.stopUtc == null) return { key: 'active', label: 'กำลังบันทึก', tone: 'info', pending: false };
  if (trip.sent) return { key: 'sent', label: 'ส่งแล้ว', tone: 'ok', pending: false };
  const tr = s.transfers[trip.trip_id];
  const base = { pending: true, transfer: tr };
  switch (tr?.stage) {
    case 'receiving': return { ...base, key: 'receiving', label: `กำลังรับข้อมูล ${pct(tr)}`, tone: 'info' };
    case 'ble_interrupted': return { ...base, key: 'ble_interrupted', label: `รับไม่ครบ ${pct(tr)} · รอเชื่อมต่อ`, tone: 'warn' };
    case 'received':
    case 'csv':
    case 'ready_upload': return { ...base, key: 'in_app', label: 'รับเข้าแอปครบแล้ว', tone: 'info' };
    case 'waiting_internet': return { ...base, key: 'waiting_internet', label: 'อยู่ในแอป · รออินเทอร์เน็ต', tone: 'warn' };
    case 'uploading':
    case 'awaiting_server': return { ...base, key: 'uploading', label: 'กำลังส่งขึ้น Server', tone: 'info' };
    case 'upload_failed': return { ...base, key: 'upload_failed', label: 'ส่ง Server ไม่สำเร็จ', tone: 'bad' };
    case 'server_ok':
    case 'marking':
    case 'mark_failed': return { ...base, key: 'server_ok', label: 'Server รับแล้ว · รอยืนยันกับอุปกรณ์', tone: 'warn' };
    default: return { ...base, key: 'pending', label: 'รอส่ง', tone: 'warn' };
  }
}
const pct = (tr) => `${Math.floor((100 * tr.rowsReceived) / Math.max(1, tr.totalRows))}%`;

// CSV columns from ble-protocol.md §6 (ASCII, CRLF). Size = header + rows × one typical row of this trip.
const CSV_HEADER = 'trip_id,trip_date,trip_number,seq,sn,timestamp,utc,event,temp,tempmin,tempmax,alarm,timeok,gnssstate,latitude,longitude,motion,battery,internet,detail\r\n';

/** Estimated size of the trip's CSV, i.e. what the phone uploads over its internet. */
export function csvBytes(dev, trip) {
  const rows = tripSeries(dev, trip, trip.stopUtc ?? simNow()).rows;
  const hasFix = dev.gnss.fixAt != null;
  const row = [
    trip.trip_id, trip.trip_date, trip.trip_number, Math.floor(rows / 2), dev.sn,
    '14:24:00 07/10/2026', Math.floor(trip.startUtc / 1000), 'SAMPLE', '4.25', trip.low.toFixed(1), trip.high.toFixed(1),
    '', 'true', hasFix ? 'last' : 'none', hasFix ? '13.8121' : '', hasFix ? '100.5583' : '', '0', '78', 'offline', '',
  ].join(',') + '\r\n';
  return CSV_HEADER.length + rows * row.length;
}

export function pendingTrips(s, dev) {
  return dev.trips.filter((t) => t.stopUtc != null && !t.sent);
}

// ---------------------------------------------------------------------------
// Connection (NFC tap → find → BLE → AUTH)
// ---------------------------------------------------------------------------
export function finishConnect(sn) {
  update((s) => {
    const now = simNow();
    if (s.ble && s.ble.sn !== sn) {
      const prev = findDevice(s, s.ble.sn);
      if (prev) prev.lastSyncAt = now;
    }
    s.ble = { sn, authorized: true, since: now };
    s.selectedSn = sn;
    const dev = findDevice(s, sn);
    if (dev.pendingFirstRead) {
      Object.assign(dev, dev.pendingFirstRead);
      delete dev.pendingFirstRead;
    }
    dev.lastSyncAt = now;
  });
}

export function disconnect() {
  update((s) => {
    if (!s.ble) return;
    const dev = findDevice(s, s.ble.sn);
    if (dev) dev.lastSyncAt = simNow();
    s.ble = null;
  });
}

function requireLink(sn) {
  if (!isLinked(getState(), sn)) throw new BleLost('before');
}

/** One-shot fault: drop the BLE link in the middle of the next command. */
function takeDropFlag() {
  const on = getState().demo.bleDropNext;
  if (on) update((s) => void (s.demo.bleDropNext = false));
  return on;
}

// ---------------------------------------------------------------------------
// Trip commands
// ---------------------------------------------------------------------------
function boxStartTrip(s, sn, cmd) {
  const dev = findDevice(s, sn);
  if (dev.seenReq[cmd.reqId]) return dev.seenReq[cmd.reqId]; // idempotent retry, same id → same answer
  const now = simNow();
  let res;
  if (dev.activeTripId) res = { ok: false, err: 'ALREADY_ACTIVE' };
  else if (dev.batteryMv != null && dev.batteryMv < BATT_TRIP_MV) res = { ok: false, err: 'BATTERY_LOW' };
  else if (dev.storagePct >= 99) res = { ok: false, err: 'LOG_FULL' };
  else {
    const diff = dev.clockDiffS;
    const adjusted = Math.abs(diff) > 5;
    dev.clockDiffS = 0;
    if (adjusted || dev.timeQuality === 'rtc') dev.timeQuality = 'host';
    const date = tripDate(now, dev.config.tz_offset_min);
    const trip = {
      trip_id: uuid(),
      trip_date: date,
      trip_number: dev.trips.filter((t) => t.trip_date === date).length + 1,
      startUtc: now,
      stopUtc: null,
      low: cmd.args.low,
      high: cmd.args.high,
      period: dev.config.sample_period_s,
      dwell: dev.config.alarm_dwell_s,
      hyst: dev.config.alarm_hyst_c10,
      events: [{ type: 'TRIP_START', t: now, temp: tempAt(dev, now) }],
      sent: false,
    };
    dev.trips.push(trip);
    dev.activeTripId = trip.trip_id;
    res = { ok: true, trip_id: trip.trip_id, clock: { diff_s: diff, adjusted, limited: false, quality: 'host' } };
  }
  dev.seenReq[cmd.reqId] = res;
  return res;
}

function boxStopTrip(s, sn, cmd) {
  const dev = findDevice(s, sn);
  if (dev.seenReq[cmd.reqId]) return dev.seenReq[cmd.reqId];
  let res;
  if (!dev.activeTripId) res = { ok: false, err: 'NOT_ACTIVE' };
  else {
    const now = simNow();
    const trip = dev.trips.find((t) => t.trip_id === dev.activeTripId);
    trip.stopUtc = now;
    trip.events.push({ type: 'TRIP_STOP', t: now, temp: tempAt(dev, now) });
    dev.activeTripId = null;
    res = { ok: true, trip_id: trip.trip_id };
  }
  dev.seenReq[cmd.reqId] = res;
  return res;
}

/**
 * Send a trip command. The request id is kept until the box answers, so a retry after BLE loss
 * re-sends the *same* id and the box answers with the original result instead of acting twice.
 */
async function tripCommand(sn, kind, args, boxFn) {
  requireLink(sn);
  let cmd = getState().pendingCmd[sn];
  if (!cmd || cmd.kind !== kind) {
    cmd = { kind, reqId: nextReqId(), args: { ...args, utc: Math.floor(simNow() / 1000) } };
    update((s) => void (s.pendingCmd[sn] = cmd));
  }
  await wait(1100);
  const drop = takeDropFlag();
  let res;
  update((s) => {
    res = boxFn(s, sn, cmd); // the command reached the box…
  });
  if (drop || !isLinked(getState(), sn)) {
    disconnect();
    throw new BleLost('after'); // …but the answer never came back
  }
  update((s) => {
    delete s.pendingCmd[sn];
    findDevice(s, sn).lastSyncAt = simNow();
  });
  return res;
}

export function pendingCommand(s, sn, kind) {
  const c = s.pendingCmd[sn];
  return c && c.kind === kind ? c : null;
}

export function clearPendingCommand(sn) {
  update((s) => void delete s.pendingCmd[sn]);
}

export async function startTrip(sn, { low, high, name, note }) {
  const res = await tripCommand(sn, 'START', { low, high }, boxStartTrip);
  if (res.ok) {
    update((s) => {
      const cur = s.appTrips[res.trip_id] || {};
      // a retry with the same request id returns the same trip: keep the first "started by"
      s.appTrips[res.trip_id] = { name: name ?? cur.name ?? '', note: note ?? cur.note ?? '', ...cur, startedBy: cur.startedBy || stamp(s) };
      if (name) s.appTrips[res.trip_id].name = name;
      if (note) s.appTrips[res.trip_id].note = note;
    });
  }
  return res;
}

export async function stopTrip(sn) {
  const res = await tripCommand(sn, 'STOP', {}, boxStopTrip);
  if (res.ok) {
    update((s) => {
      const cur = s.appTrips[res.trip_id] || { name: '', note: '' };
      s.appTrips[res.trip_id] = { ...cur, stoppedBy: cur.stoppedBy || stamp(s) };
    });
  }
  return res;
}

export async function ackAlarm(sn) {
  requireLink(sn);
  await wait(800);
  if (takeDropFlag()) {
    disconnect();
    throw new BleLost('during');
  }
  update((s) => {
    const dev = findDevice(s, sn);
    const trip = dev.trips.find((t) => t.trip_id === dev.activeTripId);
    if (!trip) return;
    const st = stamp(s);
    trip.events.push({ type: 'ALARM_ACK', t: st.t });
    const meta = s.appTrips[trip.trip_id] || { name: '', note: '' };
    s.appTrips[trip.trip_id] = { ...meta, acks: [...(meta.acks || []), st] }; // who acknowledged: app data
  });
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
export const CONFIG_RULES = {
  sample_period_s: { min: 60, max: 3600 },
  temp_adj_c100: { min: -1000, max: 1000 },
  upload_period_s: { min: 60, max: 86400 },
  gnss_period_s: { min: 300, max: 86400 },
  tz_offset_min: { min: -720, max: 840 },
  led_front_pct: { min: 1, max: 100 },
  led_bright_pct: { min: 1, max: 100 },
  buzzer_enabled: { min: 0, max: 1 },
  alarm_dwell_s: { min: 0, max: 3600 },
  alarm_hyst_c10: { min: 0, max: 100 },
  idle_wake_s: { min: 300, max: 86400 },
  led_status_s: { min: 60, max: 3600 },
  accel_wake_ths: { min: 1, max: 63 },
  usb_drive: { min: 0, max: 1 },
};

/** Save changed values (APPLY_CONFIG). Returns { applied: [keys], errors: { key: message } }. */
export async function applyConfig(sn, changes) {
  requireLink(sn);
  await wait(1200);
  const mode = getState().demo.settingsResult;
  if (mode !== 'ok') update((s) => void (s.demo.settingsResult = 'ok'));
  if (mode === 'fail' || takeDropFlag() || !isLinked(getState(), sn)) {
    disconnect();
    throw new BleLost('during');
  }
  const applied = [];
  const errors = {};
  const keys = Object.keys(changes);
  const rejectKey = mode === 'partial' ? keys[keys.length - 1] : null;
  update((s) => {
    const dev = findDevice(s, sn);
    for (const k of keys) {
      const v = changes[k];
      const rule = CONFIG_RULES[k];
      if (dev.activeTripId && TRIP_LOCKED.includes(k)) errors[k] = 'มีทริปกำลังทำงาน แก้ไขได้หลังจบทริป';
      else if (!rule || !Number.isFinite(v) || v < rule.min || v > rule.max) errors[k] = 'ค่าอยู่นอกช่วงที่อุปกรณ์รับได้';
      else if (k === rejectKey && keys.length > 1) errors[k] = 'อุปกรณ์ไม่รับค่านี้ ลองบันทึกอีกครั้ง';
      else {
        dev.config[k] = v;
        applied.push(k);
      }
    }
    if (keys.length === 1 && rejectKey) {
      // a single change cannot be "partial": treat as a rejection of that one key
      const k = keys[0];
      if (applied.includes(k)) {
        applied.splice(applied.indexOf(k), 1);
        errors[k] = 'อุปกรณ์ไม่รับค่านี้ ลองบันทึกอีกครั้ง';
      }
    }
    dev.lastSyncAt = simNow();
  });
  if (applied.length) update((s) => void (findDevice(s, sn).lastConfig = { ...stamp(s), keys: applied }));
  return { applied, errors };
}

/** ADD_WIFI / EDIT_WIFI / DEL_WIFI. Password is never stored in or returned to the app. */
export async function wifiCommand(sn, op, data) {
  requireLink(sn);
  await wait(900);
  if (takeDropFlag() || !isLinked(getState(), sn)) {
    disconnect();
    throw new BleLost('during');
  }
  let out = { ok: true };
  update((s) => {
    const dev = findDevice(s, sn);
    const slots = dev.wifi;
    const net = (w) => {
      const n = { slot: w.slot, ssid: data.ssid, dhcp: data.dhcp };
      if (!data.dhcp) Object.assign(n, { ip: data.ip, gateway: data.gateway, subnet: data.subnet, dns: data.dns || '' });
      return n;
    };
    if (op === 'add') {
      const dup = slots.find((w) => w.ssid && w.ssid === data.ssid);
      const target = slots.find((w) => w.slot === data.slot) || slots.find((w) => !w.ssid);
      if (dup) out = { ok: false, err: 'EXISTS', msg: `มีเครือข่ายชื่อนี้อยู่แล้วในช่อง ${dup.slot}` };
      else if (!target) out = { ok: false, err: 'FULL', msg: 'บันทึกครบ 5 เครือข่ายแล้ว ลบเครือข่ายเดิมก่อน' };
      else if (target.ssid) out = { ok: false, err: 'EXISTS', msg: `ช่อง ${target.slot} มีเครือข่ายอยู่แล้ว` };
      else slots[target.slot - 1] = net(target);
    } else if (op === 'edit') {
      const cur = slots[data.slot - 1];
      const dup = slots.find((w) => w.ssid && w.ssid === data.ssid && w.slot !== data.slot);
      if (!cur?.ssid) out = { ok: false, err: 'NOT_FOUND', msg: 'ช่องนี้ว่างอยู่' };
      else if (dup) out = { ok: false, err: 'BAD_ARGS', msg: `เครือข่ายนี้อยู่ในช่อง ${dup.slot} แล้ว` };
      else slots[data.slot - 1] = net(cur);
    } else if (op === 'del') {
      const cur = slots[data.slot - 1];
      if (!cur?.ssid) out = { ok: false, err: 'NOT_FOUND', msg: 'ช่องนี้ว่างอยู่' };
      else slots[data.slot - 1] = { slot: data.slot, ssid: null };
    }
    dev.lastSyncAt = simNow();
  });
  if (out.ok) update((s) => void (findDevice(s, sn).lastConfig = { ...stamp(s), keys: [`wifi ${op} ${data.slot}`] }));
  return out;
}

// ---------------------------------------------------------------------------
// Send trips: box → BLE → app → phone internet → server → MARK_DELIVERED
// ---------------------------------------------------------------------------
function setTransfer(id, patch) {
  update((s) => {
    s.transfers[id] = { ...s.transfers[id], ...patch, updatedAt: simNow() };
  });
}

async function processTrip(sn, id) {
  let s = getState();
  const dev = findDevice(s, sn);
  const trip = dev.trips.find((t) => t.trip_id === id);
  if (!trip || trip.stopUtc == null || trip.sent) return;
  if (!s.transfers[id]) {
    const total = tripSeries(dev, trip, trip.stopUtc).rows;
    setTransfer(id, { sn, tripId: id, stage: 'queued', rowsReceived: 0, totalRows: total, csv: null, uploads: 0, error: null, requestedBy: getState().userId });
  }
  let tr = getState().transfers[id];

  // 1) READ_LOG_CHUNK until `next` is null. Rows are stored by seq, so a resume never duplicates.
  if (['queued', 'receiving', 'ble_interrupted'].includes(tr.stage)) {
    if (!isLinked(getState(), sn)) {
      setTransfer(id, { stage: 'ble_interrupted', error: 'ไม่ได้เชื่อมต่อกับอุปกรณ์' });
      return;
    }
    setTransfer(id, { stage: 'receiving', error: null });
    const total = tr.totalRows;
    const chunkDelay = Math.max(25, Math.min(140, 3500 / Math.ceil(total / CHUNK_ROWS)));
    const dropAt = takeDropFlag() ? Math.floor(total * 0.45) : null;
    let got = tr.rowsReceived;
    while (got < total) {
      await wait(chunkDelay);
      if (!isLinked(getState(), sn) || (dropAt != null && got >= dropAt)) {
        if (isLinked(getState(), sn)) disconnect();
        setTransfer(id, { stage: 'ble_interrupted', rowsReceived: got, error: 'การเชื่อมต่อ Bluetooth หลุดระหว่างรับข้อมูล' });
        return;
      }
      got = Math.min(total, got + CHUNK_ROWS);
      setTransfer(id, { rowsReceived: got });
    }
    setTransfer(id, { stage: 'received' });
    await wait(500);
  }

  // 2) Build the CSV in the app
  tr = getState().transfers[id];
  if (tr.stage === 'received') {
    setTransfer(id, { stage: 'csv' });
    await wait(700);
    setTransfer(id, { stage: 'ready_upload', csv: csvName(sn, trip.startUtc, dev.config.tz_offset_min) });
  }

  // 3–4) Upload with the phone's internet and wait for the server to confirm storage
  tr = getState().transfers[id];
  if (['ready_upload', 'waiting_internet', 'upload_failed'].includes(tr.stage)) {
    if (!getState().internet) {
      setTransfer(id, { stage: 'waiting_internet', error: 'มือถือไม่มีอินเทอร์เน็ต ข้อมูลเก็บไว้ในแอปแล้ว' });
      return;
    }
    setTransfer(id, { stage: 'uploading', error: null, uploads: (tr.uploads || 0) + 1 });
    await wait(1300);
    if (!getState().internet) {
      setTransfer(id, { stage: 'waiting_internet', error: 'อินเทอร์เน็ตหลุดระหว่างส่ง ข้อมูลยังอยู่ในแอป' });
      return;
    }
    setTransfer(id, { stage: 'awaiting_server' });
    await wait(1000);
    if (getState().demo.serverMode === 'fail') {
      setTransfer(id, { stage: 'upload_failed', error: 'Server ไม่ยืนยันการรับข้อมูล ข้อมูลยังอยู่ในแอป' });
      return;
    }
    setTransfer(id, { stage: 'server_ok', serverAt: simNow() });
    await wait(400);
  }

  // 5) Tell the box (MARK_DELIVERED) — only after the server confirmed
  tr = getState().transfers[id];
  if (['server_ok', 'mark_failed'].includes(tr.stage)) {
    if (!isLinked(getState(), sn)) {
      setTransfer(id, { stage: 'mark_failed', error: 'ต้องเชื่อมต่อกับอุปกรณ์เพื่อยืนยัน' });
      return;
    }
    setTransfer(id, { stage: 'marking', error: null });
    await wait(900);
    const fail = getState().demo.markFailNext;
    if (fail) update((s) => void (s.demo.markFailNext = false));
    if (fail || !isLinked(getState(), sn)) {
      setTransfer(id, { stage: 'mark_failed', error: 'อุปกรณ์ยังไม่ตอบรับการยืนยัน' });
      return;
    }
    update((s) => {
      const d = findDevice(s, sn);
      d.trips.find((t) => t.trip_id === id).sent = true;
      d.lastSyncAt = simNow();
    });
    setTransfer(id, { stage: 'done', error: null, doneAt: simNow(), sentBy: getState().userId });
  }
}

/** Process trips one at a time. Retrying only touches trips that are not done. */
export async function runBatch(ids) {
  if (getState().batch?.running) return;
  update((s) => void (s.batch = { ids, running: true }));
  try {
    for (const id of ids) {
      const tr = getState().transfers[id];
      if (tr?.stage === 'done') continue;
      const dev = getState().devices.find((d) => d.trips.some((t) => t.trip_id === id));
      if (dev) await processTrip(dev.sn, id);
    }
  } finally {
    update((s) => void (s.batch = { ids, running: false }));
  }
  const s = getState();
  const done = ids.filter((id) => s.transfers[id]?.stage === 'done').length;
  if (done === ids.length) toast(`ส่งถึง Server และยืนยันกับอุปกรณ์แล้ว ${done} ทริป`, 'ok');
  else toast(`สำเร็จ ${done} จาก ${ids.length} ทริป · ที่เหลือยังค้างอยู่`, 'warn');
}

// ---------------------------------------------------------------------------
// Demo controls
// ---------------------------------------------------------------------------
export function setInternet(on) {
  update((s) => void (s.internet = on));
  if (on) {
    const waiting = Object.values(getState().transfers).filter((t) => t.stage === 'waiting_internet').map((t) => t.tripId);
    if (waiting.length) {
      toast(`มีอินเทอร์เน็ตแล้ว · กำลังส่งข้อมูลที่ค้างในแอป ${waiting.length} ทริป`, 'info');
      setTimeout(() => runBatch(waiting), 400);
    }
  }
}

function pushTripEvent(dev, ev) {
  const trip = dev.activeTripId && dev.trips.find((t) => t.trip_id === dev.activeTripId);
  if (trip) trip.events.push(ev);
}

export function demoToggleHigh(sn) {
  update((s) => {
    const dev = findDevice(s, sn);
    const now = simNow();
    const open = dev.episodes.find((e) => e.type === 'high' && e.to == null);
    if (open) {
      open.to = now;
      pushTripEvent(dev, { type: 'ALARM_CLEAR', t: now, detail: 'ALARM_HIGH', temp: tempAt(dev, now + 1) });
    } else {
      dev.episodes.push({ type: 'high', from: now, to: null });
      // The prototype skips the alarm delay so the alarm shows at once.
      pushTripEvent(dev, { type: 'ALARM_HIGH', t: now, temp: tempAt(dev, now) });
    }
  });
}

export function demoToggleProbe(sn) {
  update((s) => {
    const dev = findDevice(s, sn);
    const now = simNow();
    const open = dev.episodes.find((e) => e.type === 'probe' && e.to == null);
    if (open) {
      open.to = now;
      pushTripEvent(dev, { type: 'ALARM_CLEAR', t: now, detail: 'ALARM_PROBE' });
    } else {
      dev.episodes.push({ type: 'probe', from: now, to: null });
      pushTripEvent(dev, { type: 'ALARM_PROBE', t: now });
    }
  });
}

export function demoToggleBattery(sn) {
  update((s) => {
    const dev = findDevice(s, sn);
    const now = simNow();
    if (dev.lowBattery) {
      Object.assign(dev, dev.savedBattery || { battery: 70, batteryMv: 3900 });
      dev.lowBattery = false;
      pushTripEvent(dev, { type: 'ALARM_CLEAR', t: now, detail: 'BATTERY_LOW' });
    } else {
      dev.savedBattery = { battery: dev.battery ?? 70, batteryMv: dev.batteryMv ?? 3900 };
      dev.battery = 9;
      dev.batteryMv = 3480;
      dev.lowBattery = true;
      pushTripEvent(dev, { type: 'BATTERY_LOW', t: now, detail: '9' });
    }
  });
}

export function demoAdvance(ms) {
  update((s) => {
    s.clockOffsetMs += ms;
    const now = Date.now() + s.clockOffsetMs;
    for (const dev of s.devices) {
      if (!dev.activeTripId) continue;
      // During a trip the box looks for a position every gnss_period_s.
      const base = dev.gnss.lat ? dev.gnss : { lat: 13.7563, lon: 100.5018 };
      dev.gnss = { fixAt: now - 3 * MIN, lat: +(base.lat + 0.01).toFixed(4), lon: +(base.lon + 0.012).toFixed(4) };
      if (dev.battery != null && !dev.lowBattery) dev.battery = Math.max(15, dev.battery - Math.round(ms / (6 * HOUR)));
    }
    if (s.ble) {
      const d = findDevice(s, s.ble.sn);
      if (d) d.lastSyncAt = now;
    }
  });
}
