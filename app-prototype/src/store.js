// Tiny global store for the prototype. Everything is mock data kept in the browser.
// Persisted to localStorage (wrapped in try/catch) so the demo survives reloads; "รีเซ็ตข้อมูลจำลอง" clears it.
import { useSyncExternalStore } from 'react';

const STORAGE_KEY = 'cch-prototype-state-v1';
const SCHEMA = 5;

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
  idle_wake_s: 3600,
  led_status_s: 900,
  accel_wake_ths: 2,
  usb_drive: 1,
  // test-only, set from the USB console; the app shows them read-only
  tap_test: 0,
  rx_show: 0,
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
    // GET_INFO / NDEF identity. MACs follow the ESP32 pattern of the doc example (BLE = Wi-Fi MAC + 2).
    proto: 1,
    wifiMac: o.wifiMac,
    bleMac: o.bleMac,
    timeQuality: o.timeQuality || 'rtc',
    serverRev: o.serverRev ?? null, // SETTINGS.server_rev: last settings document applied from the server
    otaBase: o.otaBase ?? null,
    wifiNow: o.wifiNow || { connected: false },
    knownSince: o.knownSince,
  };
}

const devName = (sn) => `mCOLD ${sn.slice(-3)}`;

/**
 * Mock fleet. A box is not tied to one shipping box: it is clipped onto whatever box goes out,
 * so devices are named by their own number and the cargo is recorded per trip (app-only label).
 */
export function seed(now = Date.now()) {
  const sn = (n) => `mCDV1-L0169-1069-${String(n).padStart(3, '0')}`;
  const hex = (v) => v.toString(16).toUpperCase().padStart(2, '0');
  const mac = (low) => `28:84:85:27:9B:${hex(low)}`;
  const mk = (n, o) =>
    mkDevice({ sn: sn(n), name: devName(sn(n)), knownSince: now - 20 * DAY, wifiMac: mac(0x80 + (n - 1) * 4), bleMac: mac(0x82 + (n - 1) * 4), ...o });
  const appTrips = {};

  const d1 = mk(1, {
    baseTemp: 4.6, battery: 86, batteryMv: 3990, storagePct: 3, clockDiffS: 59, lastSyncAt: now - 18 * MIN,
    wifi: [{ slot: 1, ssid: 'Warehouse-2', dhcp: true }, { slot: 3, ssid: 'Office', dhcp: false, ip: '192.168.1.50', gateway: '192.168.1.1', subnet: '255.255.255.0', dns: '8.8.8.8' }],
  });
  mkTrip(d1, { start: now - 4 * DAY - 3 * HOUR, stop: now - 3 * DAY - 2 * HOUR, sent: true });

  // mCDV1-L0169-1069-002 is the example box in the docs; its values are taken from them:
  // NDEF ble MAC, STATUS (soc 78, 3987 mV, used_pct 1, time ntp), WIFI event, GET_NETWORK 5 slots,
  // and the rev 3 settings document (upload_period_s 600, temp_adj_c100 -50, ota_base).
  const d2 = mk(2, {
    baseTemp: 4.75, battery: 78, batteryMv: 3987, storagePct: 1, lastSyncAt: now - 4 * MIN,
    gnss: { fixAt: now - 25 * MIN, lat: 13.8121, lon: 100.5583 },
    wifiMac: '28:84:85:27:9B:84', bleMac: '28:84:85:27:9B:86', timeQuality: 'ntp', serverRev: 3,
    otaBase: 'https://drive.siamatic.co.th/media/firmwares',
    config: { upload_period_s: 600, temp_adj_c100: -50 },
    wifi: [
      { slot: 1, ssid: 'RDE_2.4GHz', dhcp: true },
      { slot: 3, ssid: 'Warehouse', dhcp: false, ip: '192.168.1.50', gateway: '192.168.1.1', subnet: '255.255.255.0', dns: '' },
    ],
    wifiNow: { connected: true, ssid: 'RDE_2.4GHz', dhcp: true, ip: '192.168.0.13', gateway: '192.168.0.1', subnet: '255.255.255.0', dns: '192.168.0.1', rssi: -51 },
  });
  mkTrip(d2, { start: now - 9 * DAY, stop: now - 7 * DAY - 5 * HOUR, sent: true });
  mkTrip(d2, { start: now - 6 * DAY - 4 * HOUR, stop: now - 5 * DAY, sent: true });
  const hiFrom = now - 9 * HOUR - 6 * MIN;
  const hiTo = now - 8 * HOUR - 20 * MIN;
  d2.episodes.push({ type: 'high', from: hiFrom, to: hiTo });
  const t2 = mkTrip(d2, {
    start: now - 26 * HOUR - 20 * MIN,
    events: [
      { type: 'ALARM_HIGH', t: hiFrom + 5 * MIN, temp: 10.4 },
      { type: 'ALARM_ACK', t: hiFrom + 16 * MIN },
      { type: 'ALARM_CLEAR', t: hiTo, detail: 'ALARM_HIGH' },
    ],
  });
  d2.activeTripId = t2.trip_id;
  appTrips[t2.trip_id] = { name: 'กล่องวัคซีน #12', note: '' };

  const d3 = mk(3, {
    baseTemp: 3.8, battery: 41, batteryMv: 3760, storagePct: 18, lastSyncAt: now - 2 * HOUR - 10 * MIN,
    gnss: { fixAt: now - 3 * HOUR, lat: 13.7466, lon: 100.5347 },
    wifi: [{ slot: 2, ssid: 'Hospital-Logistics', dhcp: true }],
    timeQuality: 'gnss',
  });
  mkTrip(d3, { start: now - 12 * DAY, stop: now - 10 * DAY, sent: true });
  mkTrip(d3, { start: now - 6 * DAY - 2 * HOUR, stop: now - 3 * DAY - 16 * HOUR, low: 2, high: 6 });
  mkTrip(d3, { start: now - 3 * DAY - 2 * HOUR, stop: now - 2 * DAY - 22 * HOUR });
  mkTrip(d3, { start: now - 20 * HOUR, stop: now - 11 * HOUR, low: 2, high: 6 });

  const d4 = mk(4, { baseTemp: 24.8, battery: 95, batteryMv: 4120, storagePct: 1, lastSyncAt: now - 1 * DAY });

  const d5 = mk(5, { baseTemp: 25.1, battery: 12, batteryMv: 3500, storagePct: 2, lastSyncAt: now - 40 * MIN });
  d5.lowBattery = true;
  d5.savedBattery = { battery: 80, batteryMv: 3950 };

  const d6 = mk(6, { baseTemp: 4.2, battery: 77, batteryMv: 3930, storagePct: 4, lastSyncAt: now - 35 * MIN, gnss: { fixAt: now - 50 * MIN, lat: 13.7012, lon: 100.6034 } });
  const t6 = mkTrip(d6, { start: now - 3 * HOUR - 10 * MIN, low: 2, high: 8 });
  d6.activeTripId = t6.trip_id;
  appTrips[t6.trip_id] = { name: 'กล่องเลือด #7', note: '' };

  const d7 = mk(7, { baseTemp: 25.4, battery: 78, batteryMv: 3940, storagePct: 2, lastSyncAt: now - 3 * HOUR });
  const d8 = mk(8, { baseTemp: 25.0, battery: 91, batteryMv: 4060, storagePct: 1, lastSyncAt: now - 5 * HOUR });
  d8.episodes.push({ type: 'probe', from: now - 6 * HOUR, to: null }); // probe unplugged
  const d9 = mk(9, { baseTemp: 25.6, lastSyncAt: null, knownSince: now - 1 * DAY });
  const d10 = mk(10, { baseTemp: 25.2, lastSyncAt: null, knownSince: now - 1 * DAY });
  // Never read by the app: values stay unknown until the first connection.
  d9.pendingFirstRead = { battery: 92, batteryMv: 4080, storagePct: 0 };
  d10.pendingFirstRead = { battery: 88, batteryMv: 4040, storagePct: 0 };

  return {
    schema: SCHEMA,
    seededAt: now,
    clockOffsetMs: 0,
    selectedSn: d1.sn,
    internet: true,
    ble: null, // { sn, authorized, since }
    demo: {
      nfcOutcome: 'ok', // ok | notfound | connfail | authfail
      bleDropNext: false,
      serverMode: 'ok', // ok | fail
      markFailNext: false,
      settingsResult: 'ok', // ok | partial | fail
    },
    devices: [d1, d2, d3, d4, d5, d6, d7, d8, d9, d10],
    appTrips, // trip_id -> { name, note } (app-only data, not sent to the box)
    transfers: {}, // trip_id -> transfer job state in the app queue
    batch: null, // { ids, running }
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
  /**
   * Jump to a function screen of the connected box ('device' = its menu, 'devices' = device list),
   * dropping flow screens above it: products › mCOLD › device › function.
   */
  toTab: (tab) =>
    updateUi(() => {
      const base = [{ name: 'products' }, { name: 'mcold' }];
      if (tab === 'devices') return { route: base };
      base.push({ name: 'device' });
      if (tab !== 'device') base.push({ name: tab });
      return { route: base };
    }),
};

let toastId = 0;
export function toast(text, tone = 'info') {
  const id = ++toastId;
  updateUi((u) => ({ toasts: [...u.toasts, { id, text, tone }] }));
  setTimeout(() => updateUi((u) => ({ toasts: u.toasts.filter((t) => t.id !== id) })), 3200);
}
