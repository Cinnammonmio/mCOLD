// Start many boxes in one go: set the trip once, then tap each box. Every tap is its own
// NFC → BLE → authorize → check → START_TRIP → disconnect, so the phone never has to hold
// many BLE links at once and the next box is ready right away. (UX proposal, see the guide.)
import { useRef, useState } from 'react';
import { currentUser, getState, nav, toast, update, useApp, wardDevices, wardOf } from '../store.js';
import { BleLost, disconnect, findDevice, finishConnect, startTrip } from '../sim.js';
import { fmtTime } from '../format.js';
import { AppBar, Btn, Chip, Icon, Seg } from '../ui.jsx';
import { ERR_TEXT, preflight } from './TripFlows.jsx';

const wait = (ms) => new Promise((r) => setTimeout(r, ms));

const NFC_FAIL = {
  notfound: 'ไม่พบอุปกรณ์ แตะค้างไว้ 1–2 วินาทีแล้วลองใหม่',
  connfail: 'เชื่อมต่อไม่สำเร็จ แตะเครื่องนี้อีกครั้ง',
  authfail: 'ยืนยันสิทธิ์ไม่ผ่าน แตะเครื่องนี้อีกครั้ง',
};

export function MultiStart() {
  const s = useApp();
  const [form, setForm] = useState({ low: '2.0', high: '8.0' });
  const [rows, setRows] = useState([]); // newest first: { sn, state, step, msg, tripId, at }
  const rowsRef = useRef([]);
  const [busy, setBusy] = useState(false);
  const busyRef = useRef(false);
  const [picked, setPicked] = useState('');

  const low = parseFloat(form.low);
  const high = parseFloat(form.high);
  const errs = {};
  if (!Number.isFinite(low)) errs.low = 'กรอกตัวเลข';
  if (!Number.isFinite(high)) errs.high = 'กรอกตัวเลข';
  if (!errs.low && !errs.high && low >= high) errs.high = 'ค่าสูงสุดต้องมากกว่าค่าต่ำสุด';
  const formOk = Object.keys(errs).length === 0;

  const setRow = (sn, patch) => {
    const cur = rowsRef.current;
    const i = cur.findIndex((r) => r.sn === sn);
    const next = i >= 0 ? cur.map((r, j) => (j === i ? { ...r, ...patch } : r)) : [{ sn, ...patch }, ...cur];
    // a re-tapped box moves back to the top
    if (i > 0) next.unshift(next.splice(i, 1)[0]);
    rowsRef.current = next;
    setRows(next);
  };

  /** Demo: the next box the user would pick up, i.e. one not handled yet and without a running trip. */
  const nextBox = () => {
    const st = getState();
    const handled = (sn) => rowsRef.current.some((r) => r.sn === sn);
    const mine = wardDevices(st);
    return (mine.find((d) => !handled(d.sn) && !d.activeTripId) || mine.find((d) => !handled(d.sn)))?.sn;
  };
  const target = picked || nextBox() || wardDevices(s)[0]?.sn || s.devices[0].sn;

  const tapOne = async (sn) => {
    if (busyRef.current) return;
    if (rowsRef.current.some((r) => r.sn === sn && r.state === 'started')) {
      toast(`${findDevice(getState(), sn).name} เริ่มทริปไปแล้วในรอบนี้`, 'info');
      return;
    }
    busyRef.current = true;
    setBusy(true);
    try {
      setRow(sn, { state: 'working', step: 'กำลังเชื่อมต่อ…', msg: null, at: Date.now() });
      const outcome = getState().demo.nfcOutcome;
      if (outcome !== 'ok') update((st) => void (st.demo.nfcOutcome = 'ok'));
      await wait(800);
      if (outcome !== 'ok') {
        setRow(sn, { state: 'failed', msg: NFC_FAIL[outcome] });
        return;
      }
      if (findDevice(getState(), sn).wardId !== currentUser(getState()).wardId) {
        setRow(sn, { state: 'failed', msg: `เครื่องนี้ลงทะเบียนกับ${wardOf(getState(), findDevice(getState(), sn).wardId).name}` });
        return;
      }
      finishConnect(sn);
      setRow(sn, { step: 'ตรวจความพร้อม…' });
      await wait(500);
      const st = getState();
      const dev = findDevice(st, sn);
      const retrying = st.pendingCmd[sn]?.kind === 'START';
      if (dev.activeTripId && !retrying) {
        setRow(sn, { state: 'skipped', msg: 'มีทริปทำงานอยู่แล้ว ไม่เริ่มซ้ำ' });
        disconnect();
        return;
      }
      const pf = preflight(st, dev);
      const block = pf.checks.find((c) => c.state === 'block' && c.key !== 'trip');
      if (block && !retrying) {
        setRow(sn, { state: 'failed', msg: `${block.title}: ${block.note}` });
        disconnect();
        return;
      }
      setRow(sn, { step: 'กำลังเริ่มทริป…' });
      const res = await startTrip(sn, { low, high });
      if (res.ok) setRow(sn, { state: 'started', tripId: res.trip_id, msg: null });
      else setRow(sn, { state: 'failed', msg: ERR_TEXT[res.err]?.title || 'อุปกรณ์ไม่เริ่มทริป' });
      disconnect(); // free the link; the next box is a new tap anyway
    } catch (e) {
      if (e instanceof BleLost) setRow(sn, { state: 'unknown', msg: 'ไม่ได้รับการยืนยัน แตะเครื่องนี้อีกครั้งเพื่อตรวจสอบ (จะไม่เริ่มซ้ำ)' });
      else throw e;
    } finally {
      busyRef.current = false;
      setBusy(false);
      setPicked('');
    }
  };

  const tapMany = async (n) => {
    for (let i = 0; i < n; i++) {
      const sn = nextBox();
      if (!sn) break;
      // eslint-disable-next-line no-await-in-loop
      await tapOne(sn);
      // eslint-disable-next-line no-await-in-loop
      await wait(250);
    }
  };

  const started = rows.filter((r) => r.state === 'started').length;
  const failed = rows.filter((r) => r.state === 'failed' || r.state === 'unknown').length;
  const skipped = rows.filter((r) => r.state === 'skipped').length;

  const finish = () => {
    if (started) toast(`เริ่มทริปแล้ว ${started} เครื่อง`, 'ok');
    nav.toTab('devices');
  };

  const locked = started > 0 || busy; // the range is fixed once the first box has started
  const rangeInput = (key, label) => (
    <div className="input-unit" style={{ flex: 1 }}>
      <input
        id={`m-${key}`}
        aria-label={label}
        className={`input num ${errs[key] ? 'invalid' : ''}`}
        inputMode="decimal"
        value={form[key]}
        disabled={locked}
        onChange={(e) => setForm({ ...form, [key]: e.target.value })}
        style={{ fontSize: 18, fontWeight: 600, minHeight: 46 }}
      />
      <span className="unit">°C</span>
    </div>
  );

  return (
    <div className="screen">
      <AppBar title="เริ่มทริปหลายเครื่อง" sub={`เริ่มแล้ว ${started} · ไม่สำเร็จ ${failed} · ข้าม ${skipped}`} onBack={() => (rows.length ? finish() : nav.back())} />
      <div className="content stack with-footer" style={{ gap: 10 }}>
        <div className="card stack tight" style={{ padding: 12 }}>
          <div className="row" style={{ gap: 8 }}>
            {rangeInput('low', 'ต่ำสุด')}
            <span className="muted">–</span>
            {rangeInput('high', 'สูงสุด')}
          </div>
          <div className="tiny" style={{ color: formOk ? 'var(--muted)' : 'var(--bad)' }}>
            {formOk ? (locked ? 'ใช้ช่วงนี้กับทุกเครื่องในรอบนี้' : 'ตั้งช่วงครั้งเดียว แล้วแตะ NFC ทีละเครื่อง') : errs.low || errs.high}
          </div>
        </div>

        <div className="stack tight" style={{ border: '1px dashed #cfc2f0', borderRadius: 12, padding: 8, background: 'var(--demo-bg)' }}>
          <div className="row" style={{ gap: 6 }}>
            <Icon name="flask-2" size={14} style={{ color: 'var(--demo)', flex: 'none' }} />
            <select id="m-pick" aria-label="เครื่องที่แตะ (จำลอง)" className="input" value={target} onChange={(e) => setPicked(e.target.value)} style={{ minHeight: 36, padding: '4px 8px', fontSize: 14 }}>
              {s.devices.map((d) => (
                <option key={d.sn} value={d.sn}>{d.name}{d.activeTripId ? ' (มีทริปอยู่)' : ''}{d.wardId !== currentUser(s).wardId ? ' · วอร์ดอื่น' : ''}</option>
              ))}
            </select>
            <Btn kind="demo" size="sm" disabled={busy || !formOk} onClick={() => tapMany(5)} style={{ flex: 'none' }}>แตะต่อ 5</Btn>
          </div>
          <Seg
            demo
            label="ผลการแตะจำลอง"
            value={s.demo.nfcOutcome}
            onChange={(v) => update((st) => void (st.demo.nfcOutcome = v))}
            options={[{ value: 'ok', label: 'สำเร็จ' }, { value: 'notfound', label: 'ไม่พบ' }, { value: 'connfail', label: 'ต่อไม่ได้' }, { value: 'authfail', label: 'สิทธิ์ไม่ผ่าน' }]}
          />
        </div>

        {rows.length > 0 ? (
          <div className="list">
            {rows.map((r) => <ResultRow key={r.sn} r={r} onRetry={() => tapOne(r.sn)} busy={busy} />)}
          </div>
        ) : (
          <div className="center small muted" style={{ padding: '18px 0' }}>
            <Icon name="nfc" size={28} /><br />แตะ NFC บนกล่องทีละเครื่อง แอปตรวจความพร้อมและเริ่มทริปให้ทันที
          </div>
        )}
      </div>
      <div className="footer-bar row" style={{ gap: 8 }}>
        <Btn kind="demo" className="grow" icon={busy ? undefined : 'nfc'} busy={busy} disabled={!formOk} onClick={() => tapOne(target)} style={{ minHeight: 52, background: 'var(--demo)', color: '#fff' }}>
          {busy ? 'กำลังทำงาน…' : 'จำลองแตะ NFC'}
        </Btn>
        <Btn kind="secondary" disabled={busy} onClick={finish} style={{ minHeight: 52 }}>
          เสร็จสิ้น
        </Btn>
      </div>
    </div>
  );
}

/** One line per tapped box; a cargo field appears under boxes that started. */
function ResultRow({ r, onRetry, busy }) {
  const s = useApp();
  const dev = findDevice(s, r.sn);
  const trip = r.tripId ? dev.trips.find((t) => t.trip_id === r.tripId) : null;
  const label = trip ? s.appTrips[trip.trip_id]?.name || '' : '';
  const chip = {
    working: <Chip tone="info" icon="loader-2">{r.step}</Chip>,
    started: <Chip tone="ok" icon="circle-check">เริ่มแล้ว {trip ? fmtTime(trip.startUtc) : ''}</Chip>,
    skipped: <Chip tone="outline">ข้าม</Chip>,
    failed: <Chip tone="bad" icon="alert-triangle">ไม่สำเร็จ</Chip>,
    unknown: <Chip tone="warn" icon="alert-triangle">ไม่ทราบผล</Chip>,
  }[r.state];
  return (
    <div className="stack tight" style={{ padding: '8px 12px', background: 'var(--surface)' }}>
      <div className="row between">
        <span className="bold">{dev.name}</span>
        {chip}
      </div>
      {r.msg && (
        <div className="row small" style={{ gap: 6 }}>
          <span className="grow" style={{ color: r.state === 'skipped' ? 'var(--muted)' : 'var(--bad)' }}>{r.msg}</span>
          {(r.state === 'failed' || r.state === 'unknown') && (
            <Btn kind="secondary" size="sm" icon="nfc" disabled={busy} onClick={onRetry} style={{ flex: 'none', minHeight: 32 }}>แตะอีกครั้ง</Btn>
          )}
        </div>
      )}
      {trip && (
        <input
          className="input"
          style={{ minHeight: 34, fontSize: 13, padding: '4px 10px' }}
          aria-label={`กล่องที่ติด ${dev.name}`}
          placeholder="กล่องที่ติด (ไม่บังคับ)"
          maxLength={40}
          value={label}
          onChange={(e) => update((st) => void (st.appTrips[trip.trip_id] = { ...(st.appTrips[trip.trip_id] || { note: '' }), name: e.target.value }))}
        />
      )}
    </div>
  );
}
