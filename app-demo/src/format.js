// Display helpers. Unknown values always render as "—", never 0.

export const DASH = '—';

const isNum = (v) => typeof v === 'number' && Number.isFinite(v);

export function fmtTemp(c, digits = 1) {
  return isNum(c) ? `${c.toFixed(digits)}°C` : DASH;
}

export function fmtNum(v, unit = '') {
  return isNum(v) ? `${v.toLocaleString('th-TH')}${unit}` : DASH;
}

export function fmtPct(v) {
  return isNum(v) ? `${Math.round(v)}%` : DASH;
}

export function fmtTime(ms) {
  if (!isNum(ms)) return DASH;
  return new Date(ms).toLocaleTimeString('th-TH', { hour: '2-digit', minute: '2-digit' });
}

export function fmtDate(ms) {
  if (!isNum(ms)) return DASH;
  return new Date(ms).toLocaleDateString('th-TH', { day: 'numeric', month: 'short', year: 'numeric' });
}

export function fmtDateShort(ms) {
  if (!isNum(ms)) return DASH;
  return new Date(ms).toLocaleDateString('th-TH', { day: 'numeric', month: 'short' });
}

export function fmtDateTime(ms) {
  if (!isNum(ms)) return DASH;
  return `${fmtDateShort(ms)} ${fmtTime(ms)}`;
}

export function fmtDuration(ms) {
  if (!isNum(ms) || ms < 0) return DASH;
  const mins = Math.floor(ms / 60000);
  const d = Math.floor(mins / 1440);
  const h = Math.floor((mins % 1440) / 60);
  const m = mins % 60;
  if (d > 0) return `${d} วัน ${h} ชม.`;
  if (h > 0) return `${h} ชม. ${m} นาที`;
  return `${m} นาที`;
}

export function fmtAgo(ms, now) {
  if (!isNum(ms)) return DASH;
  const s = Math.max(0, Math.round((now - ms) / 1000));
  if (s < 60) return 'เมื่อสักครู่';
  const m = Math.round(s / 60);
  if (m < 60) return `${m} นาทีที่แล้ว`;
  const h = Math.round(m / 60);
  if (h < 48) return `${h} ชม.ที่แล้ว`;
  return `${Math.round(h / 24)} วันที่แล้ว`;
}

export function fmtPeriod(sec) {
  if (!isNum(sec)) return DASH;
  if (sec % 3600 === 0) return `${sec / 3600} ชม.`;
  if (sec % 60 === 0) return `${sec / 60} นาที`;
  return `${sec} วินาที`;
}

export function fmtTz(min) {
  if (!isNum(min)) return DASH;
  const sign = min >= 0 ? '+' : '−';
  const a = Math.abs(min);
  const h = Math.floor(a / 60);
  const m = a % 60;
  return `UTC${sign}${h}${m ? ':' + String(m).padStart(2, '0') : ''}`;
}

/** Human trip label, e.g. "ทริปที่ 2 · 11 ต.ค. 2569". trip_id stays internal. */
export function tripLabel(trip) {
  if (!trip) return DASH;
  return `ทริปที่ ${trip.trip_number} · ${fmtDate(trip.startUtc)}`;
}

/** CSV file name per ble-protocol.md §6: TRIP_<SN>_<YYMMDDhhmm>.csv in local time of the box. */
export function csvName(sn, startUtc, tzMin = 420) {
  const d = new Date(startUtc + tzMin * 60000);
  const p = (n) => String(n).padStart(2, '0');
  const s = `${p(d.getUTCFullYear() % 100)}${p(d.getUTCMonth() + 1)}${p(d.getUTCDate())}${p(d.getUTCHours())}${p(d.getUTCMinutes())}`;
  return `TRIP_${sn}_${s}.csv`;
}
