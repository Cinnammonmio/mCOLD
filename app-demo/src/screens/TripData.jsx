// Trip data per box: list of trips → one trip as a graph and a table of its rows.
// Rows come from the box over Bluetooth (READ_LOG_CHUNK, works for finished and running trips);
// a trip already received for sending is shown from the app without reading it again.
import { useEffect, useRef, useState } from 'react';
import { nav, useApp, simNow, userName } from '../store.js';
import { CHUNK_ROWS, deviceView, findDevice, isLinked, tripDelivery, tripRows, tripSeries, alarmState } from '../sim.js';
import { fmtDateShort, fmtDateTime, fmtDuration, fmtNum, fmtTemp, fmtTime, tripLabel } from '../format.js';
import { requestConnect } from '../ConnectSheet.jsx';
import { AppBar, Btn, Chip, Icon, Seg } from '../ui.jsx';
import { TempChart, eventText } from './Trip.jsx';

const wait = (ms) => new Promise((r) => setTimeout(r, ms));
const readCache = new Set(); // trip_ids read into the app during this session

export function TripsScreen() {
  const s = useApp();
  const dev = findDevice(s, s.selectedSn);
  const v = deviceView(s, dev);
  const trips = [...dev.trips].reverse().filter((t) => t.startUtc <= (v.at ?? 0));
  return (
    <div className="screen">
      <AppBar title="ข้อมูลทริป" sub={dev.name} backLabel="เมนู" onBack={() => nav.toTab('device')} />
      <div className="content stack" style={{ gap: 10 }}>
        <div className="tiny muted">ทริปที่เก็บอยู่ในอุปกรณ์ แตะเพื่อดูกราฟและตาราง</div>
        <div className="list">
          {trips.map((t) => {
            const d = tripDelivery(s, dev, t);
            const ser = tripSeries(dev, t, t.stopUtc ?? v.at);
            const out = (ser.min != null && ser.min < t.low) || (ser.max != null && ser.max > t.high);
            return (
              <button key={t.trip_id} className="li" onClick={() => nav.push({ name: 'trip-detail', tripId: t.trip_id })} style={{ padding: '10px 12px' }}>
                <div className="grow" style={{ minWidth: 0 }}>
                  <div className="row between" style={{ gap: 6 }}>
                    <span className="bold" style={{ fontSize: 14, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{s.appTrips[t.trip_id]?.name || tripLabel(t)}</span>
                    <Chip tone={d.tone} dot={d.key === 'active'}>{d.key === 'active' ? 'กำลังบันทึก' : d.key === 'sent' ? 'ส่งแล้ว' : 'รอส่ง'}</Chip>
                  </div>
                  <div className="tiny muted">
                    {fmtDateShort(t.startUtc)} {fmtTime(t.startUtc)} · {fmtDuration((t.stopUtc ?? v.at) - t.startUtc)} · {fmtNum(ser.rows)} แถว
                  </div>
                  <div className="tiny" style={{ color: out ? 'var(--bad)' : 'var(--muted)' }}>
                    ช่วง {fmtTemp(t.low)}–{fmtTemp(t.high)} · ต่ำ/สูง {fmtTemp(ser.min)} / {fmtTemp(ser.max)}{out ? ' · มีช่วงนอกเกณฑ์' : ''}
                  </div>
                </div>
                <Icon name="chevron-right" className="muted" />
              </button>
            );
          })}
          {trips.length === 0 && <div className="li small muted" style={{ justifyContent: 'center' }}>ยังไม่มีทริปในอุปกรณ์นี้</div>}
        </div>
      </div>
    </div>
  );
}

export function TripDetail({ tripId }) {
  const s = useApp();
  const dev = s.devices.find((d) => d.trips.some((t) => t.trip_id === tripId));
  const trip = dev.trips.find((t) => t.trip_id === tripId);
  const linked = isLinked(s, dev.sn);
  const inApp = ['received', 'csv', 'ready_upload', 'waiting_internet', 'uploading', 'awaiting_server', 'upload_failed', 'server_ok', 'marking', 'mark_failed', 'done'].includes(s.transfers[tripId]?.stage);
  const [have, setHave] = useState(inApp || (readCache.has(tripId) && trip.stopUtc != null));
  const [readAt, setReadAt] = useState(() => (have ? simNow() : null));
  const [progress, setProgress] = useState(null);
  const [view, setView] = useState('chart');
  const [only, setOnly] = useState('all');
  const [limit, setLimit] = useState(150);
  const alive = useRef(true);
  useEffect(() => () => void (alive.current = false), []);

  const until = trip.stopUtc ?? readAt ?? simNow();
  const rows = have ? tripRows(dev, trip, until) : [];
  const ser = tripSeries(dev, trip, until);
  const al = alarmState(trip, until);
  const meta = s.appTrips[tripId];

  // READ_LOG_CHUNK, 16 rows a time, until `next` is null
  const read = async () => {
    const total = tripSeries(dev, trip, trip.stopUtc ?? simNow()).rows;
    setProgress(0);
    for (let got = 0; got < total; got += CHUNK_ROWS * 4) {
      await wait(30);
      if (!alive.current) return;
      setProgress(Math.min(total, got));
    }
    readCache.add(tripId);
    setReadAt(simNow());
    setHave(true);
    setProgress(null);
  };
  useEffect(() => {
    if (!have && linked) read();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [linked]);

  const shown = only === 'all' ? rows : rows.filter((r) => r.ev || (r.temp != null && (r.temp < trip.low || r.temp > trip.high)) || r.temp == null);
  const row = (r) => {
    const out = r.temp != null && (r.temp < trip.low || r.temp > trip.high);
    const alarm = ['ALARM_HIGH', 'ALARM_LOW', 'ALARM_PROBE', 'BATTERY_LOW'].includes(r.type);
    return (
      <tr key={r.seq} style={{ color: alarm || out ? 'var(--bad)' : undefined, fontWeight: r.ev ? 600 : 400 }}>
        <td className="num">{r.seq}</td>
        <td className="num">{fmtDateShort(r.t)} {fmtTime(r.t)}</td>
        <td className="num" style={{ textAlign: 'right' }}>{r.temp == null ? '—' : r.temp.toFixed(2)}</td>
        <td>{r.type === 'SAMPLE' ? <span className="muted">บันทึก</span> : eventText(r)}</td>
      </tr>
    );
  };

  return (
    <div className="screen">
      <AppBar title={meta?.name || tripLabel(trip)} sub={`${dev.name}${meta?.name ? ` · ${tripLabel(trip)}` : ''}`} backLabel="ทริป" />
      <div className="content stack" style={{ gap: 10 }}>
        <div className="card" style={{ padding: 12 }}>
          <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr 1fr', gap: '8px 10px' }}>
            {[
              ['เริ่ม', `${fmtDateShort(trip.startUtc)} ${fmtTime(trip.startUtc)}`],
              ['จบ', trip.stopUtc ? `${fmtDateShort(trip.stopUtc)} ${fmtTime(trip.stopUtc)}` : 'กำลังบันทึก'],
              ['ระยะเวลา', fmtDuration(until - trip.startUtc)],
              ['ช่วงที่อนุญาต', `${fmtTemp(trip.low)}–${fmtTemp(trip.high)}`],
              ['ต่ำ / สูง', `${fmtTemp(ser.min)} / ${fmtTemp(ser.max)}`, (ser.min != null && ser.min < trip.low) || (ser.max != null && ser.max > trip.high)],
              ['แจ้งเตือน', `${al.raised} ครั้ง`, al.raised > 0],
              ['ข้อมูล', `${fmtNum(ser.rows)} แถว`],
              ['เริ่มโดย', meta?.startedBy ? userName(s, meta.startedBy.by).split(' ')[0] : '—'],
              ['จบโดย', meta?.stoppedBy ? userName(s, meta.stoppedBy.by).split(' ')[0] : '—'],
            ].map(([k, val, bad]) => (
              <div key={k} style={{ minWidth: 0 }}>
                <div className="tiny muted" style={{ fontSize: 11 }}>{k}</div>
                <div className="small bold num" style={{ color: bad ? 'var(--bad)' : undefined }}>{val}</div>
              </div>
            ))}
          </div>
        </div>

        {!have && progress == null && !linked && (
          <div className="banner neutral">
            <Icon name="bluetooth-off" />
            <div className="grow small">
              <b>ต้องเชื่อมต่อเพื่ออ่านข้อมูลทริปจากอุปกรณ์</b>
              <div className="actions"><Btn size="sm" kind="teal" icon="nfc" onClick={() => requestConnect(dev.sn)}>แตะ NFC เพื่อเชื่อมต่อ</Btn></div>
            </div>
          </div>
        )}
        {progress != null && (
          <div className="card stack tight" style={{ padding: 12 }}>
            <div className="small"><Icon name="loader-2" size={14} className="spin" /> กำลังอ่านข้อมูลจากอุปกรณ์ผ่าน Bluetooth {fmtNum(progress)} / {fmtNum(ser.rows)} แถว</div>
            <div className="progress"><div style={{ width: `${(100 * progress) / Math.max(1, ser.rows)}%` }} /></div>
          </div>
        )}

        {have && (
          <>
            <div className="row" style={{ gap: 8 }}>
              <div className="grow">
                <Seg label="มุมมอง" value={view} onChange={setView} options={[{ value: 'chart', label: 'กราฟ' }, { value: 'table', label: 'ตาราง' }]} />
              </div>
              {trip.stopUtc == null && linked && (
                <Btn size="sm" kind="secondary" icon="refresh" onClick={read}>อัปเดต</Btn>
              )}
            </div>
            <div className="tiny muted">
              {inApp ? 'ข้อมูลที่รับเข้าแอปแล้ว' : `อ่านจากอุปกรณ์เมื่อ ${fmtTime(readAt)}`}
              {trip.stopUtc == null ? ' · ทริปยังบันทึกอยู่' : ''}
            </div>
            {view === 'chart' ? (
              <div className="card stack tight" style={{ padding: 10 }}>
                <TempChart trip={{ ...trip, ...tripSeries(dev, trip, until) }} current={undefined} at={until} />
                <div className="row tiny muted wrap" style={{ gap: 12 }}>
                  <span className="row" style={{ gap: 4 }}><span style={{ width: 14, height: 8, background: 'var(--ok-bg)', border: '1px solid #b9dfcb', borderRadius: 2 }} /> ช่วงที่อนุญาต</span>
                  <span className="row" style={{ gap: 4 }}><span style={{ width: 14, height: 2, background: 'var(--teal)' }} /> อุณหภูมิ</span>
                  <span className="row" style={{ gap: 4 }}><span style={{ width: 7, height: 7, borderRadius: 4, background: 'var(--bad)' }} /> นอกช่วง</span>
                  <span className="row" style={{ gap: 4 }}><span style={{ width: 4, height: 10, background: '#f3d7a6' }} /> ไม่มีค่า</span>
                </div>
                <div className="stack tight" style={{ marginTop: 4 }}>
                  {rows.filter((r) => r.ev).map((r) => (
                    <div key={r.seq} className="row tiny" style={{ gap: 6 }}>
                      <span className="num muted" style={{ width: 82, flex: 'none' }}>{fmtDateShort(r.t)} {fmtTime(r.t)}</span>
                      <span style={{ color: ['ALARM_HIGH', 'ALARM_LOW', 'ALARM_PROBE', 'BATTERY_LOW'].includes(r.type) ? 'var(--bad)' : undefined }}>{eventText(r)}{r.temp != null ? ` · ${fmtTemp(r.temp)}` : ''}</span>
                    </div>
                  ))}
                </div>
              </div>
            ) : (
              <>
                <Seg label="แสดงแถว" value={only} onChange={(v) => { setOnly(v); setLimit(150); }} options={[{ value: 'all', label: `ทุกแถว ${fmtNum(rows.length)}` }, { value: 'events', label: 'เหตุการณ์ / นอกช่วง' }]} />
                <div className="tbl-wrap" style={{ borderRadius: 12 }}>
                  <table className="tbl" style={{ minWidth: 0, fontSize: 13 }}>
                    <thead>
                      <tr><th>#</th><th>เวลา</th><th style={{ textAlign: 'right' }}>°C</th><th>เหตุการณ์</th></tr>
                    </thead>
                    <tbody>{shown.slice(0, limit).map(row)}</tbody>
                  </table>
                </div>
                {shown.length > limit && (
                  <Btn kind="secondary" block onClick={() => setLimit(limit + 300)}>แสดงเพิ่ม ({fmtNum(shown.length - limit)} แถว)</Btn>
                )}
                {shown.length === 0 && <div className="small muted center">ไม่มีเหตุการณ์หรือค่านอกช่วง</div>}
              </>
            )}
          </>
        )}
      </div>
    </div>
  );
}
