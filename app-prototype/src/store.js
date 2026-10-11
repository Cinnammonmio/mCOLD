// Tiny global store for the prototype. Everything is mock data kept in the browser.
// Persisted to localStorage (wrapped in try/catch) so the demo survives reloads; "รีเซ็ตข้อมูลจำลอง" clears it.
import { useSyncExternalStore } from 'react';

const STORAGE_KEY = 'cch-prototype-state-v1';
const SCHEMA = 3;

export const DEFAULT_CONFIG = {
  sample_period_s: 300,
  temp_adj_c100: 0,
  upload_period_s: 300,
  gnss_period_s: 1800,
  tz_offset_min: 420,
  led_front_pct: 2,
  led_bright_pct: 20,
  buzzer_enabled: 1,
  alarm_dwell_s: 300,
  alarm_hyst_c10: 5,
};

/** Keys the docs lock while a trip runs (device-settings.md, 🔒). */
export const TRIP_LOCKED = ['sample_period_s', 'temp_adj_c100', 'alarm_dwell_s', 'alarm_hyst_c10'];

export function uuid() {
  if (globalThis.crypto?.randomUUID) return crypto.randomUUID();
  return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, (c) => {
    const r = (Math.random() * 16) | 0;
    return (c === 'x' ? r : (r & 0x3) | 0x8).toString(16);
  });
}

let reqCounter = Math.floor(Math.random() * 1000) + 100;
export const nextReqId = () => ++reqCounter;

const MIN = 60000;
const HOUR = 60 * MIN;
const DAY = 24 * HOUR;

export function tripDate(ms, tzMin = 420) {
  const d = new Date(ms + tzMin * MIN);
  const p = (n) => String(n).padStart(2, '0');
  return `${d.getUTCFullYear()}${p(d.getUTCMonth() + 1)}${p(d.getUTCDate())}`;
}

function mkTrip(dev, { start, stop = null, low = 2, high = 8, sent = false, events = [] }) {
  const date = tripDate(start, dev.config.tz_offset_min);
  const sameDay = dev.trips.filter((t) => t.trip_date === date).length;
  const trip = {
    trip_id: uuid(),
    trip_date: date,
    trip_number: sameDay + 1,
    startUtc: start,
    stopUtc: stop,
    low,
    high,
    period: dev.config.sample_period_s,
    dwell: dev.config.alarm_dwell_s,
    hyst: dev.config.alarm_hyst_c10,
    events: [{ type: 'TRIP_START', t: start }, ...events],
    sent,
  };
  if (stop) trip.events.push({ type: 'TRIP_STOP', t: stop });
  dev.trips.push(trip);
  return trip;
}

function wifiSlots(list) {
  return [1, 2, 3, 4, 5].map((slot) => list.find((w) => w.slot === slot) || { slot, ssid: null });
}

function mkDevice(o) {
  return {
    sn: o.sn,
    name: o.name,
    fw: '0.7.0-dev.50',
    hw: 'foamV1.0.1',
    baseTemp: o.baseTemp,
    battery: o.battery ?? null,
    batteryMv: o.batteryMv ?? null,
    lowBattery: false,
    storagePct: o.storagePct ?? 1,
    clockDiffS: o.clockDiffS ?? 2,
    episodes: [],
    config: { ...DEFAULT_CONFIG, ...(o.config || {}) },
    wifi: wifiSlots(o.wifi || []),
    gnss: o.gnss || { fixAt: null, lat: null, lon: null },
    activeTripId: null,
    trips: [],
    seenReq: {},
    lastSyncAt: o.lastSyncAt ?? null,
    knownSince: o.knownSince,
  };
}

export function seed(now = Date.now()) {
  const A = mkDevice({
    sn: 'mCDV1-L0169-1069-001', name: 'กล่องยา A-01', baseTemp: 4.6, battery: 86, batteryMv: 3990,
    storagePct: 3, clockDiffS: 59, lastSyncAt: now - 18 * MIN, knownSince: now - 20 * DAY,
    wifi: [{ slot: 1, ssid: 'Warehouse-2', dhcp: true }, { slot: 3, ssid: 'Office', dhcp: false, ip: '192.168.1.50', gateway: '192.168.1.1', subnet: '255.255.255.0', dns: '8.8.8.8' }],
  });
  mkTrip(A, { start: now - 4 * DAY - 3 * HOUR, stop: now - 3 * DAY - 2 * HOUR, sent: true });

  const B = mkDevice({
    sn: 'mCDV1-L0169-1069-002', name: 'กล่องวัคซีน B-02', baseTemp: 5.1, battery: 64, batteryMv: 3870,
    storagePct: 7, lastSyncAt: now - 4 * MIN, knownSince: now - 30 * DAY,
    gnss: { fixAt: now - 25 * MIN, lat: 13.8121, lon: 100.5583 },
    wifi: [{ slot: 1, ssid: 'Warehouse-2', dhcp: true }],
  });
  mkTrip(B, { start: now - 9 * DAY, stop: now - 7 * DAY - 5 * HOUR, sent: true });
  mkTrip(B, { start: now - 6 * DAY - 4 * HOUR, stop: now - 5 * DAY, sent: true });
  const hiFrom = now - 9 * HOUR - 6 * MIN;
  const hiTo = now - 8 * HOUR - 20 * MIN;
  B.episodes.push({ type: 'high', from: hiFrom, to: hiTo });
  const active = mkTrip(B, {
    start: now - 26 * HOUR - 20 * MIN,
    events: [
      { type: 'ALARM_HIGH', t: hiFrom + 5 * MIN, temp: 10.4 },
      { type: 'ALARM_ACK', t: hiFrom + 16 * MIN },
      { type: 'ALARM_CLEAR', t: hiTo, detail: 'ALARM_HIGH' },
    ],
  });
  B.activeTripId = active.trip_id;

  const C = mkDevice({
    sn: 'mCDV1-L0169-1069-003', name: 'กล่องเลือด C-03', baseTemp: 3.8, battery: 41, batteryMv: 3760,
    storagePct: 18, lastSyncAt: now - 2 * HOUR - 10 * MIN, knownSince: now - 25 * DAY,
    gnss: { fixAt: now - 3 * HOUR, lat: 13.7466, lon: 100.5347 },
    wifi: [{ slot: 2, ssid: 'Hospital-Logistics', dhcp: true }],
  });
  mkTrip(C, { start: now - 12 * DAY, stop: now - 10 * DAY, sent: true });
  mkTrip(C, { start: now - 6 * DAY - 2 * HOUR, stop: now - 3 * DAY - 16 * HOUR, low: 2, high: 6 });
  mkTrip(C, { start: now - 3 * DAY - 2 * HOUR, stop: now - 2 * DAY - 22 * HOUR });
  mkTrip(C, { start: now - 20 * HOUR, stop: now - 11 * HOUR, low: 2, high: 6 });

  const D = mkDevice({ sn: 'mCDV1-L0169-1069-004', name: 'กล่องสำรอง D-04', baseTemp: 26.4, lastSyncAt: null, knownSince: now - 1 * DAY });
  // The app has never read this box: values stay unknown until the first connection.
  D.pendingFirstRead = { battery: 92, batteryMv: 4080, storagePct: 0 };

  return {
    schema: SCHEMA,
    seededAt: now,
    clockOffsetMs: 0,
    selectedSn: B.sn,
    mcoldTab: 'devices',
    internet: true,
    ble: null, // { sn, authorized, since }
    demo: {
      nfcOutcome: 'ok', // ok | notfound | connfail | authfail
      bleDropNext: false,
      serverMode: 'ok', // ok | fail
      markFailNext: false,
      settingsResult: 'ok', // ok | partial | fail
    },
    devices: [A, B, C, D],
    appTrips: {}, // trip_id -> { name, note } (app-only data, not sent to the box)
    transfers: {}, // trip_id -> transfer job state in the app queue
    batch: null, // { sn, ids, running }
    pendingCmd: {}, // sn -> { kind, reqId, args } for idempotent retry after BLE loss
  };
}

function load() {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return null;
    const s = JSON.parse(raw);
    if (s.schema !== SCHEMA) return null;
    // A page reload ends every BLE session and every running job.
    s.ble = null;
    if (s.batch) s.batch.running = false;
    for (const t of Object.values(s.transfers || {})) {
      if (t.stage === 'receiving') t.stage = 'ble_interrupted';
      if (t.stage === 'csv') t.stage = 'received';
      if (t.stage === 'uploading' || t.stage === 'awaiting_server') t.stage = 'upload_failed';
      if (t.stage === 'marking') t.stage = 'mark_failed';
    }
    return s;
  } catch {
    return null;
  }
}

function save(s) {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(s));
  } catch {
    /* storage blocked: the demo still works for this session */
  }
}

let state = load() || seed();
// UI-only state that should not survive a reload.
let ui = { route: [{ name: 'products' }], toasts: [], connect: null, demoOpen: false };
let snapshot = { ...state, ui };
const listeners = new Set();

function emit() {
  snapshot = { ...state, ui };
  listeners.forEach((l) => l());
}

export function getState() {
  return state;
}

/** Mutate a deep copy of the persisted state. */
export function update(fn) {
  const draft = structuredClone(state);
  fn(draft);
  state = draft;
  save(state);
  emit();
}

export function updateUi(fn) {
  ui = { ...ui, ...fn(ui) };
  emit();
}

export function resetAll() {
  try {
    localStorage.removeItem(STORAGE_KEY);
  } catch {
    /* ignore */
  }
  state = seed();
  save(state);
  ui = { route: [{ name: 'products' }], toasts: [], connect: null, demoOpen: false };
  emit();
}

const subscribe = (l) => {
  listeners.add(l);
  return () => listeners.delete(l);
};

export function useApp() {
  return useSyncExternalStore(subscribe, () => snapshot);
}

export const simNow = () => Date.now() + state.clockOffsetMs;

// ---- navigation -----------------------------------------------------------
export const nav = {
  push: (r) => updateUi((u) => ({ route: [...u.route, r] })),
  replace: (r) => updateUi((u) => ({ route: [...u.route.slice(0, -1), r] })),
  back: () => updateUi((u) => ({ route: u.route.length > 1 ? u.route.slice(0, -1) : u.route })),
  reset: (r) => updateUi(() => ({ route: [r] })),
  /** Go back to the mCOLD shell on a given tab, dropping flow screens above it. */
  toTab: (tab) => {
    update((s) => {
      s.mcoldTab = tab;
    });
    updateUi((u) => {
      const i = u.route.findIndex((r) => r.name === 'mcold');
      const base = i >= 0 ? u.route.slice(0, i + 1) : [{ name: 'products' }, { name: 'mcold' }];
      return { route: base };
    });
  },
};

let toastId = 0;
export function toast(text, tone = 'info') {
  const id = ++toastId;
  updateUi((u) => ({ toasts: [...u.toasts, { id, text, tone }] }));
  setTimeout(() => updateUi((u) => ({ toasts: u.toasts.filter((t) => t.id !== id) })), 3200);
}
