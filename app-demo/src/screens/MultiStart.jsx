// Start many boxes in one go: set the trip once, then tap each box. Every tap is its own
// NFC → BLE → authorize → check → START_TRIP → disconnect, so the phone never has to hold
// many BLE links at once and the next box is ready right away. (UX proposal, see the guide.)
import { useRef, useState } from 'react';
import { currentUser, getState, nav, toast, update, useApp, wardDevices, wardOf } from '../store.js';
import { BleLost, disconnect, findDevice, finishConnect, startTrip } from '../sim.js';
import { fmtTemp, fmtTime, tripLabel } from '../format.js';
import { AppBar, Banner, Btn, Chip, Field, Icon, Seg } from '../ui.jsx';
import { ERR_TEXT, preflight } from './TripFlows.jsx';

const wait = (ms) => new Promise((r) => setTimeout(r, ms));

const NFC_FAIL = {
  notfound: 'ไม่พบอุปกรณ์ แตะค้างไว้ 1–2 วินาทีแล้วลองใหม่',
  connfail: 'เชื่อมต่อไม่สำเร็จ แตะเครื่องนี้อีกครั้ง',
  authfail: 'ยืนยันสิทธิ์ไม่ผ่าน แตะเครื่องนี้อีกครั้ง',
};

export function MultiStart() {
  const s = useApp();
  const [phase, setPhase] = useState('setup'); // setup | tap
  const [form, setForm] = useState({ low: '2.0', high: '8.0' });
  const [touched, setTouched] = useState(false);
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

  if (phase === 'setup') {
    return (
      <div className="screen">
        <AppBar title="เริ่มทริปหลายเครื่อง" sub="ตั้งค่าทริปครั้งเดียว" />
        <div className="content stack with-footer">
          <div className="card stack">
            <h3>ช่วงอุณหภูมิที่อนุญาต</h3>
            <div className="grid2">
              <Field label="ต่ำสุด" htmlFor="m-low" error={touched && errs.low}>
                <div className="input-unit">
                  <input id="m-low" className="input num" inputMode="decimal" value={form.low} onChange={(e) => setForm({ ...form, low: e.target.value })} />
                  <span className="unit">°C</span>
                </div>
              </Field>
              <Field label="สูงสุด" htmlFor="m-high" error={touched && errs.high}>
                <div className="input-unit">
                  <input id="m-high" className="input num" inputMode="decimal" value={form.high} onChange={(e) => setForm({ ...form, high: e.target.value })} />
                  <span className="unit">°C</span>
                </div>
              </Field>
            </div>
            <div className="tiny muted">ใช้กับทุกเครื่องที่แตะในรอบนี้ ค่าตัวอย่าง 2–8°C</div>
          </div>
          <Banner tone="info">รอบบันทึกและค่าการแจ้งเตือนใช้ค่าที่ตั้งไว้ในแต่ละเครื่อง</Banner>
        </div>
        <div className="footer-bar">
          <Btn kind="teal" block icon="nfc" onClick={() => (setTouched(true), formOk && setPhase('tap'))}>
            ถัดไป: แตะเครื่อง
          </Btn>
        </div>
      </div>
    );
  }

  return (
    <div className="screen">
      <AppBar title="เริ่มทริปหลายเครื่อง" sub={`${fmtTemp(low)} – ${fmtTemp(high)}`} onBack={() => (rows.length ? finish() : setPhase('setup'))} />
      <div className="content stack with-footer">
        <div className="card row between">
          <div className="row" style={{ gap: 16 }}>
            <div><div className="tiny muted">เริ่มแล้ว</div><div className="bold num" style={{ fontSize: 22, color: 'var(--ok)' }}>{started}</div></div>
            <div><div className="tiny muted">ไม่สำเร็จ</div><div className="bold num" style={{ fontSize: 22, color: failed ? 'var(--bad)' : undefined }}>{failed}</div></div>
            <div><div className="tiny muted">ข้าม</div><div className="bold num" style={{ fontSize: 22 }}>{skipped}</div></div>
          </div>
          {!started && (
            <Btn kind="ghost" size="sm" icon="pencil" onClick={() => setPhase('setup')}>แก้ช่วง</Btn>
          )}
        </div>

        <div className="card stack center" style={{ alignItems: 'center' }}>
          <div className="bold row"><Icon name="nfc" size={20} /> แตะ NFC ทีละเครื่อง</div>
          <Btn kind="teal" block icon={busy ? undefined : 'nfc'} busy={busy} onClick={() => tapOne(target)}>
            {busy ? 'กำลังทำงานกับเครื่องที่แตะ…' : 'จำลองแตะ NFC'}
          </Btn>
          <div className="stack tight" style={{ width: '100%', textAlign: 'left', border: '1px dashed #cfc2f0', borderRadius: 12, padding: 10, background: 'var(--demo-bg)' }}>
            <label htmlFor="m-pick" className="tiny bold" style={{ color: 'var(--demo)' }}>
              <Icon name="flask-2" size={13} /> โหมดจำลอง · เครื่องที่แตะ
            </label>
            <select id="m-pick" className="input" value={target} onChange={(e) => setPicked(e.target.value)}>
              {s.devices.map((d) => (
                <option key={d.sn} value={d.sn}>{d.name}{d.activeTripId ? ' (มีทริปอยู่)' : ''}{d.wardId !== currentUser(s).wardId ? ' · วอร์ดอื่น' : ''}</option>
              ))}
            </select>
            <Seg
              demo
              label="ผลการแตะจำลอง"
              value={s.demo.nfcOutcome}
              onChange={(v) => update((st) => void (st.demo.nfcOutcome = v))}
              options={[{ value: 'ok', label: 'สำเร็จ' }, { value: 'notfound', label: 'ไม่พบ' }, { value: 'connfail', label: 'ต่อไม่ได้' }, { value: 'authfail', label: 'สิทธิ์ไม่ผ่าน' }]}
            />
            <Btn kind="demo" size="sm" disabled={busy} onClick={() => tapMany(5)}>จำลองแตะต่อเนื่อง 5 เครื่อง</Btn>
          </div>
        </div>

        {rows.length > 0 && <div className="section-title">เครื่องที่แตะในรอบนี้</div>}
        <div className="stack tight">
          {rows.map((r) => <ResultRow key={r.sn} r={r} onRetry={() => tapOne(r.sn)} busy={busy} />)}
        </div>
      </div>
      <div className="footer-bar">
        <Btn block icon="check" disabled={busy} onClick={finish}>
          {started ? `เสร็จสิ้น (เริ่มแล้ว ${started} เครื่อง)` : 'เสร็จสิ้น'}
        </Btn>
      </div>
    </div>
  );
}

function ResultRow({ r, onRetry, busy }) {
  const s = useApp();
  const dev = findDevice(s, r.sn);
  const trip = r.tripId ? dev.trips.find((t) => t.trip_id === r.tripId) : null;
  const label = trip ? s.appTrips[trip.trip_id]?.name || '' : '';
  const chip = {
    working: <Chip tone="info" icon="loader-2">{r.step}</Chip>,
    started: <Chip tone="ok" icon="circle-check">เริ่มทริปแล้ว</Chip>,
    skipped: <Chip tone="outline">ข้าม</Chip>,
    failed: <Chip tone="bad" icon="alert-triangle">ไม่สำเร็จ</Chip>,
    unknown: <Chip tone="warn" icon="alert-triangle">ไม่ทราบผล</Chip>,
  }[r.state];
  return (
    <div className="card stack tight" style={{ padding: 12 }}>
      <div className="row between">
        <div>
          <div className="bold">{dev.name}</div>
          <div className="tiny muted mono">{dev.sn}</div>
        </div>
        {chip}
      </div>
      {trip && <div className="tiny muted">{tripLabel(trip)} · เริ่ม {fmtTime(trip.startUtc)}</div>}
      {r.msg && <div className="small" style={{ color: r.state === 'skipped' ? 'var(--muted)' : 'var(--bad)' }}>{r.msg}</div>}
      {trip && (
        <input
          className="input"
          style={{ minHeight: 40, fontSize: 14 }}
          aria-label={`กล่องที่ติด ${dev.name}`}
          placeholder="ระบุกล่องที่ติด (ไม่บังคับ)"
          maxLength={40}
          value={label}
          onChange={(e) => update((st) => void (st.appTrips[trip.trip_id] = { ...(st.appTrips[trip.trip_id] || { note: '' }), name: e.target.value }))}
        />
      )}
      {(r.state === 'failed' || r.state === 'unknown') && (
        <Btn kind="secondary" size="sm" icon="nfc" disabled={busy} onClick={onRetry} style={{ alignSelf: 'flex-start' }}>
          แตะเครื่องนี้อีกครั้ง
        </Btn>
      )}
    </div>
  );
}
