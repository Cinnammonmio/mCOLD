// Start trip: choose box → connect/authorize → form → review → confirm → active trip.
// Stop trip: summary → choose action → confirm → result.
import { useRef, useState } from 'react';
import { nav, toast, useApp, simNow, userName } from '../store.js';
import { BATT_TRIP_MV, BleLost, clearPendingCommand, deviceView, findDevice, isLinked, pendingCommand, runBatch, startTrip, stopTrip, tripSeries } from '../sim.js';
import { fmtDateTime, fmtDuration, fmtNum, fmtPct, fmtPeriod, fmtTemp, tripLabel } from '../format.js';
import { requestConnect } from '../ConnectSheet.jsx';
import { AppBar, Banner, Btn, Chip, Field, Icon } from '../ui.jsx';
import { DeviceContext, NeedConnection } from './Mcold.jsx';

export const ERR_TEXT = {
  ALREADY_ACTIVE: { title: 'อุปกรณ์มีทริปกำลังทำงานอยู่แล้ว', fix: 'จบทริปเดิมก่อน แล้วจึงเริ่มทริปใหม่' },
  BATTERY_LOW: { title: 'แบตเตอรี่ต่ำเกินกว่าจะเริ่มทริป', fix: 'ชาร์จกล่องให้แบตเตอรี่สูงขึ้น แล้วลองอีกครั้ง' },
  LOG_FULL: { title: 'พื้นที่บันทึกในอุปกรณ์เต็ม', fix: 'ส่งข้อมูลทริปเก่าที่รอส่งก่อน เพื่อให้อุปกรณ์คืนพื้นที่' },
  NOT_ACTIVE: { title: 'อุปกรณ์ไม่มีทริปที่กำลังทำงาน', fix: 'ทริปนี้อาจจบไปแล้ว ตรวจสอบรายการรอส่ง' },
};

export function preflight(s, dev) {
  const v = deviceView(s, dev);
  const checks = [];
  const lowBatt = dev.batteryMv != null && dev.batteryMv < BATT_TRIP_MV;
  checks.push({
    key: 'battery', icon: 'battery-3', title: 'แบตเตอรี่', value: fmtPct(v.battery),
    state: lowBatt ? 'block' : v.battery != null && v.battery < 30 ? 'warn' : 'ok',
    note: lowBatt ? 'ต่ำเกินกว่าจะเริ่มทริป ชาร์จกล่องก่อน' : v.battery != null && v.battery < 30 ? 'เริ่มได้ แต่ควรชาร์จก่อนเดินทางไกล' : 'พร้อม',
  });
  checks.push({
    key: 'probe', icon: 'temperature', title: 'หัววัดอุณหภูมิ', value: v.temp == null ? 'ไม่พบ' : fmtTemp(v.temp),
    state: v.temp == null ? 'block' : 'ok',
    note: v.temp == null ? 'ตรวจว่าสายหัววัดเสียบแน่นและไม่ขาด แล้วเชื่อมต่อใหม่' : 'อ่านค่าได้ปกติ',
  });
  checks.push({
    key: 'storage', icon: 'database', title: 'พื้นที่บันทึก', value: `ใช้ไป ${fmtPct(v.storagePct)}`,
    state: v.storagePct >= 99 ? 'block' : v.storagePct >= 85 ? 'warn' : 'ok',
    note: v.storagePct >= 85 ? 'ส่งข้อมูลทริปเก่าเพื่อคืนพื้นที่' : 'พอสำหรับทริปนี้',
  });
  checks.push({
    key: 'trip', icon: 'truck-delivery', title: 'ทริปในอุปกรณ์', value: dev.activeTripId ? 'มีทริปทำงานอยู่' : 'ว่าง',
    state: dev.activeTripId ? 'block' : 'ok',
    note: dev.activeTripId ? 'หนึ่งเครื่องมีทริปทำงานได้ครั้งละหนึ่งทริป จบทริปเดิมก่อน' : 'เริ่มทริปใหม่ได้',
  });
  return { checks, blocked: checks.some((c) => c.state === 'block'), v };
}

function CheckRow({ c }) {
  const tone = c.state === 'block' ? 'bad' : c.state === 'warn' ? 'warn' : 'ok';
  return (
    <div className="li" style={{ alignItems: 'flex-start' }}>
      <Icon name={c.icon} className="muted" />
      <div className="grow">
        <div className="row between">
          <span className="t" style={{ fontSize: 14 }}>{c.title}</span>
          <Chip tone={tone} icon={c.state === 'ok' ? 'check' : 'alert-triangle'}>{c.value}</Chip>
        </div>
        <span className="s">{c.note}</span>
      </div>
    </div>
  );
}

export function StartTrip() {
  const s = useApp();
  const dev = findDevice(s, s.selectedSn);
  const linked = isLinked(s, dev.sn);
  const pend = pendingCommand(s, dev.sn, 'START');
  const [step, setStep] = useState(pend ? 3 : 1);
  const [form, setForm] = useState(() => ({
    low: pend ? String(pend.args.low) : '2.0',
    high: pend ? String(pend.args.high) : '8.0',
    name: '',
    note: '',
  }));
  const [touched, setTouched] = useState(false);
  const [busy, setBusy] = useState(false);
  const [result, setResult] = useState(pend ? { kind: 'unknown' } : null);
  const guard = useRef(false);
  const pf = preflight(s, dev);

  const low = parseFloat(form.low);
  const high = parseFloat(form.high);
  const errs = {};
  if (form.low.trim() === '' || !Number.isFinite(low)) errs.low = 'กรอกตัวเลข';
  if (form.high.trim() === '' || !Number.isFinite(high)) errs.high = 'กรอกตัวเลข';
  if (!errs.low && !errs.high && low >= high) errs.high = 'ค่าสูงสุดต้องมากกว่าค่าต่ำสุด';
  if (!errs.low && (low < -40 || low > 100)) errs.low = 'อยู่นอกช่วงที่รองรับ (−40 ถึง 100°C)';
  if (!errs.high && (high < -40 || high > 100)) errs.high = 'อยู่นอกช่วงที่รองรับ (−40 ถึง 100°C)';
  const formOk = Object.keys(errs).length === 0;

  const confirm = async () => {
    if (guard.current) return; // block double taps while the box is processing
    guard.current = true;
    setBusy(true);
    setResult(null);
    try {
      const res = await startTrip(dev.sn, { low, high, name: form.name.trim(), note: form.note.trim() });
      if (res.ok) {
        const c = res.clock;
        toast('เริ่มทริปแล้ว อุปกรณ์ยืนยันการเริ่มบันทึก', 'ok');
        if (c?.adjusted && Math.abs(c.diff_s) > 5) setTimeout(() => toast(`ตั้งเวลาของกล่องให้ตรงกับมือถือแล้ว (เดิมต่างกัน ${Math.abs(c.diff_s)} วินาที)`, 'info'), 600);
        nav.toTab('trip');
      } else {
        setResult({ kind: 'refused', err: res.err });
      }
    } catch (e) {
      if (e instanceof BleLost) setResult({ kind: e.when === 'after' ? 'unknown' : 'lost' });
      else throw e;
    } finally {
      guard.current = false;
      setBusy(false);
    }
  };

  const reconnectAndRetry = async () => {
    const ok = await requestConnect(dev.sn, 'เชื่อมต่อใหม่เพื่อตรวจสอบว่าอุปกรณ์เริ่มทริปแล้วหรือยัง');
    if (ok) confirm();
  };

  const bar = (
    <div className="wizard-steps" aria-label={`ขั้นตอน ${step} จาก 3`}>
      {[1, 2, 3].map((i) => <div key={i} className={i <= step ? 'on' : ''} />)}
    </div>
  );

  return (
    <div className="screen">
      <AppBar title="เริ่มทริป" sub={['ตรวจความพร้อม', 'กรอกข้อมูลทริป', 'ตรวจสอบและยืนยัน'][step - 1]} onBack={() => (step > 1 && !busy && result?.kind !== 'unknown' ? setStep(step - 1) : nav.back())} />
      <div className="content stack with-footer">
        {bar}
        <DeviceContext lockedReason="เปลี่ยนเครื่องได้หลังออกจากขั้นตอนนี้" />

        {step === 1 && (
          <>
            {!linked ? (
              <NeedConnection dev={dev} text="ต้องเชื่อมต่อและยืนยันสิทธิ์ก่อน แอปจึงจะอ่านสถานะเครื่องและสั่งเริ่มทริปได้" reason="เชื่อมต่อเพื่อตรวจความพร้อมก่อนเริ่มทริป" />
            ) : (
              <>
                <div className="section-title"><Icon name="list-check" size={18} /> ความพร้อมของอุปกรณ์</div>
                <div className="list">
                  {pf.checks.map((c) => <CheckRow key={c.key} c={c} />)}
                </div>
                {pf.blocked && (
                  <Banner tone="bad" title="ยังเริ่มทริปไม่ได้" actions={dev.activeTripId ? <Btn size="sm" kind="secondary" onClick={() => nav.toTab('trip')}>ไปที่ทริปที่กำลังทำงาน</Btn> : null}>
                    แก้ไขรายการที่ขึ้นสีแดงด้านบน แล้วกลับมาที่หน้านี้อีกครั้ง
                  </Banner>
                )}
                {!s.internet && <Banner tone="info" icon="world-off">มือถือไม่มีอินเทอร์เน็ต เริ่มทริปได้ตามปกติ เพราะสั่งงานกล่องผ่าน Bluetooth</Banner>}
              </>
            )}
          </>
        )}

        {step === 2 && (
          <>
            <div className="card stack">
              <h3>ช่วงอุณหภูมิที่อนุญาตของทริปนี้</h3>
              <div className="grid2">
                <Field label="ต่ำสุด" htmlFor="low" error={touched && errs.low}>
                  <div className="input-unit">
                    <input id="low" className={`input num ${touched && errs.low ? 'invalid' : ''}`} inputMode="decimal" value={form.low} onChange={(e) => setForm({ ...form, low: e.target.value })} onBlur={() => setTouched(true)} />
                    <span className="unit">°C</span>
                  </div>
                </Field>
                <Field label="สูงสุด" htmlFor="high" error={touched && errs.high}>
                  <div className="input-unit">
                    <input id="high" className={`input num ${touched && errs.high ? 'invalid' : ''}`} inputMode="decimal" value={form.high} onChange={(e) => setForm({ ...form, high: e.target.value })} onBlur={() => setTouched(true)} />
                    <span className="unit">°C</span>
                  </div>
                </Field>
              </div>
              <div className="tiny muted">ค่าตัวอย่าง 2–8°C แก้ได้ตามสินค้าที่ขนส่ง</div>
              {formOk && low < 0 && (
                <Banner tone="warn">หัววัดรุ่นนี้ยังไม่ยืนยันการวัดอุณหภูมิต่ำกว่า 0°C ตรวจสอบก่อนใช้กับสินค้าแช่แข็ง</Banner>
              )}
            </div>
            <div className="card stack tight">
              <h3>ค่าของอุปกรณ์ที่ใช้กับทริปนี้</h3>
              <dl className="kv">
                <dt>รอบบันทึกอุณหภูมิ</dt><dd>ทุก {fmtPeriod(dev.config.sample_period_s)}</dd>
                <dt>เวลาก่อนแจ้งเตือน</dt><dd>{fmtPeriod(dev.config.alarm_dwell_s) === '0 วินาที' ? 'ทันที' : fmtPeriod(dev.config.alarm_dwell_s)}</dd>
                <dt>ระยะกลับเข้าช่วง</dt><dd>{(dev.config.alarm_hyst_c10 / 10).toFixed(1)}°C</dd>
              </dl>
              <div className="tiny muted">เปลี่ยนได้ที่ ตั้งค่าอุปกรณ์ ก่อนเริ่มทริป ระหว่างทริปจะแก้ไม่ได้</div>
            </div>
            <div className="card stack">
              <h3>ข้อมูลเพิ่มเติม <span className="muted small">(ไม่บังคับ)</span></h3>
              <Field label="กล่องหรือสินค้าที่ติดเครื่องนี้" htmlFor="tname" help="เครื่องย้ายไปติดกล่องอื่นได้ทุกทริป ระบุไว้เพื่อให้รู้ว่าทริปนี้ติดกับอะไร">
                <input id="tname" className="input" maxLength={40} placeholder="เช่น กล่องวัคซีน #12" value={form.name} onChange={(e) => setForm({ ...form, name: e.target.value })} />
              </Field>
              <Field label="หมายเหตุ" htmlFor="tnote">
                <textarea id="tnote" className="input" maxLength={200} placeholder="เช่น เลขใบส่งของ ผู้รับ" value={form.note} onChange={(e) => setForm({ ...form, note: e.target.value })} />
              </Field>
            </div>
          </>
        )}

        {step === 3 && (
          <>
            <div className="card stack">
              <h3>ตรวจสอบก่อนเริ่ม</h3>
              <dl className="kv">
                <dt>อุปกรณ์</dt><dd>{dev.name}</dd>
                <dt>ช่วงที่อนุญาต</dt><dd className="num">{fmtTemp(low)} ถึง {fmtTemp(high)}</dd>
                <dt>รอบบันทึก</dt><dd>ทุก {fmtPeriod(dev.config.sample_period_s)}</dd>
                <dt>ติดกับ</dt><dd>{form.name.trim() || '—'}</dd>
                <dt>หมายเหตุ</dt><dd style={{ maxWidth: 190, overflowWrap: 'anywhere' }}>{form.note.trim() || '—'}</dd>
                <dt>เวลาเริ่ม</dt><dd>เมื่ออุปกรณ์ยืนยัน</dd>
              </dl>
              <div className="tiny muted">แอปส่งเวลาของมือถือไปด้วย ถ้านาฬิกาของกล่องคลาดเคลื่อน กล่องจะตั้งเวลาใหม่ก่อนเริ่มบันทึก</div>
            </div>
            {busy && <Banner tone="info" icon="loader-2" title="กำลังส่งคำสั่งไปยังอุปกรณ์…">รอการยืนยันจากกล่อง อย่าเพิ่งออกจากหน้านี้</Banner>}
            {result?.kind === 'refused' && (
              <Banner tone="bad" title={ERR_TEXT[result.err]?.title || 'อุปกรณ์ไม่เริ่มทริป'}>{ERR_TEXT[result.err]?.fix}</Banner>
            )}
            {result?.kind === 'lost' && (
              <Banner tone="bad" title="การเชื่อมต่อหลุดก่อนส่งคำสั่ง" actions={<Btn size="sm" kind="teal" icon="nfc" onClick={reconnectAndRetry}>เชื่อมต่อแล้วลองอีกครั้ง</Btn>}>
                อุปกรณ์ยังไม่ได้รับคำสั่ง ทริปยังไม่เริ่ม
              </Banner>
            )}
            {result?.kind === 'unknown' && (
              <Banner
                tone="warn"
                title="ไม่ได้รับการยืนยันจากอุปกรณ์"
                actions={
                  <>
                    <Btn size="sm" kind="teal" icon="nfc" onClick={reconnectAndRetry}>เชื่อมต่อและตรวจสอบอีกครั้ง</Btn>
                    <Btn size="sm" kind="secondary" onClick={() => { clearPendingCommand(dev.sn); nav.back(); }}>ยกเลิก</Btn>
                  </>
                }
              >
                การเชื่อมต่อหลุดหลังส่งคำสั่ง ยังไม่ทราบว่าทริปเริ่มแล้วหรือไม่ เชื่อมต่อใหม่เพื่อตรวจสอบ ระบบจะไม่สร้างทริปซ้ำ
              </Banner>
            )}
          </>
        )}
      </div>
      <div className="footer-bar">
        {step === 1 && (
          <Btn kind="teal" block disabled={!linked || pf.blocked} onClick={() => setStep(2)}>
            ถัดไป: กรอกข้อมูลทริป
          </Btn>
        )}
        {step === 2 && (
          <Btn kind="teal" block onClick={() => (setTouched(true), formOk && setStep(3))} aria-disabled={!formOk}>
            ถัดไป: ตรวจสอบ
          </Btn>
        )}
        {step === 3 && result?.kind !== 'unknown' && (
          <Btn kind="teal" block busy={busy} icon="player-play" disabled={!linked && !busy} onClick={confirm}>
            {busy ? 'กำลังเริ่มทริป…' : !linked ? 'เชื่อมต่อก่อนยืนยัน' : 'ยืนยันเริ่มทริป'}
          </Btn>
        )}
        {step === 3 && !linked && !busy && result?.kind !== 'unknown' && (
          <Btn kind="ghost" block icon="nfc" onClick={() => requestConnect(dev.sn)} style={{ marginTop: 4 }}>แตะ NFC เพื่อเชื่อมต่อ</Btn>
        )}
      </div>
    </div>
  );
}

export function StopTrip() {
  const s = useApp();
  const dev = findDevice(s, s.selectedSn);
  const linked = isLinked(s, dev.sn);
  const v = deviceView(s, dev);
  const pend = pendingCommand(s, dev.sn, 'STOP');
  const [choice, setChoice] = useState(pend?.args.mode || 'stop');
  const [phase, setPhase] = useState(pend ? 'unknown' : 'choose'); // choose | confirm | busy | unknown | lost | done
  const [stopped, setStopped] = useState(null);
  const [err, setErr] = useState(null);
  const guard = useRef(false);
  // Remember which trip this screen is about, so the summary stays visible if the link drops mid-command.
  const [tripId] = useState(() => v.trip?.trip_id || dev.activeTripId || dev.trips[dev.trips.length - 1]?.trip_id);
  const tripObj = dev.trips.find((t) => t.trip_id === tripId);
  const trip = v.trip?.trip_id === tripId ? v.trip : tripObj ? { ...tripObj, ...tripSeries(dev, tripObj, simNow()), alarms: null } : null;

  const doStop = async () => {
    if (guard.current) return;
    guard.current = true;
    setPhase('busy');
    setErr(null);
    try {
      const res = await stopTrip(dev.sn);
      if (!res.ok) {
        setErr(res.err);
        setPhase('choose');
        return;
      }
      setStopped(res.trip_id);
      if (choice === 'send') {
        toast('จบทริปแล้ว กำลังรับข้อมูลจากอุปกรณ์', 'ok');
        nav.replace({ name: 'send-progress', ids: [res.trip_id] });
        runBatch([res.trip_id]);
      } else {
        setPhase('done');
      }
    } catch (e) {
      if (e instanceof BleLost) setPhase(e.when === 'after' ? 'unknown' : 'lost');
      else throw e;
    } finally {
      guard.current = false;
    }
  };

  const retry = async () => {
    const ok = await requestConnect(dev.sn, 'เชื่อมต่อใหม่เพื่อยืนยันการจบทริปกับอุปกรณ์');
    if (ok) doStop();
  };

  if (phase === 'done') {
    const t = dev.trips.find((x) => x.trip_id === stopped);
    return (
      <div className="screen">
        <AppBar title="จบทริปแล้ว" back={false} />
        <div className="content stack">
          <div className="card stack center" style={{ alignItems: 'center', padding: '24px 16px' }}>
            <div className="dev-icon" style={{ width: 60, height: 60, borderRadius: 30, background: 'var(--ok-bg)', color: 'var(--ok)' }}>
              <Icon name="circle-check" size={32} />
            </div>
            <div className="bold" style={{ fontSize: 18 }}>อุปกรณ์หยุดบันทึกแล้ว</div>
            <div className="small muted">{s.appTrips[t.trip_id]?.name || tripLabel(t)} · {fmtDuration(t.stopUtc - t.startUtc)}</div>
            <div className="tiny muted">
              เริ่มโดย {userName(s, s.appTrips[t.trip_id]?.startedBy?.by)} · จบโดย {userName(s, s.appTrips[t.trip_id]?.stoppedBy?.by)}
            </div>
            <Chip tone="warn" icon="cloud-upload">รอส่ง · ข้อมูลยังเก็บในอุปกรณ์</Chip>
          </div>
          <Banner tone="info">ยังไม่ได้ส่งข้อมูลขึ้น Server ส่งได้ทุกเมื่อจากแท็บส่งข้อมูล</Banner>
        </div>
        <div className="footer-bar stack tight">
          <Btn kind="teal" block icon="cloud-upload" onClick={() => { nav.replace({ name: 'send-progress', ids: [t.trip_id] }); runBatch([t.trip_id]); }}>
            ส่งข้อมูลตอนนี้
          </Btn>
          <Btn kind="secondary" block onClick={() => nav.toTab('send')}>ดูรายการรอส่ง</Btn>
        </div>
      </div>
    );
  }

  return (
    <div className="screen">
      <AppBar title="จบทริป" sub={dev.name} onBack={phase === 'busy' ? () => {} : nav.back} />
      <div className="content stack with-footer">
        {!linked && phase !== 'unknown' && phase !== 'lost' && (
          <NeedConnection dev={dev} text="การจบทริปต้องสั่งผ่าน Bluetooth กล่องจึงหยุดบันทึก" reason="เชื่อมต่อเพื่อจบทริป" />
        )}
        {trip && (
          <div className="card stack">
            <div className="row between">
              <h3>{s.appTrips[trip.trip_id]?.name || tripLabel(trip)}</h3>
              {phase === 'unknown' || phase === 'lost' ? <Chip tone="warn">ไม่ทราบสถานะ</Chip> : <Chip tone="info" dot>กำลังบันทึก</Chip>}
            </div>
            <dl className="kv">
              <dt>เริ่ม</dt><dd>{fmtDateTime(trip.startUtc)}</dd>
              <dt>ระยะเวลา</dt><dd>{fmtDuration(simNow() - trip.startUtc)}</dd>
              <dt>ช่วงที่อนุญาต</dt><dd className="num">{fmtTemp(trip.low)} – {fmtTemp(trip.high)}</dd>
              <dt>ต่ำสุด / สูงสุด</dt><dd className="num">{fmtTemp(trip.min)} / {fmtTemp(trip.max)}</dd>
              <dt>ข้อมูลในอุปกรณ์</dt><dd className="num">{fmtNum(trip.rows)} แถว</dd>
              <dt>แจ้งเตือนทั้งทริป</dt><dd>{trip.alarms ? `${trip.alarms.raised} ครั้ง` : '—'}</dd>
            </dl>
            {trip.alarms?.active.length > 0 && <Banner tone="bad">ยังมีการแจ้งเตือนค้างอยู่ ประวัติการแจ้งเตือนจะอยู่ในข้อมูลทริป</Banner>}
          </div>
        )}

        {(phase === 'choose' || phase === 'confirm') && (
          <>
            <div className="section-title">เลือกการดำเนินการ</div>
            {[
              { id: 'stop', title: 'จบทริป', desc: 'หยุดบันทึก และเก็บข้อมูลไว้ในอุปกรณ์เพื่อส่งภายหลัง', icon: 'player-stop' },
              { id: 'send', title: 'จบทริปและส่งข้อมูล', desc: 'หยุดบันทึก แล้วรับข้อมูลทั้งทริปผ่าน Bluetooth และส่งขึ้น Server ทันที', icon: 'cloud-upload' },
            ].map((o) => (
              <label key={o.id} className={`card tap row ${choice === o.id ? 'selected' : ''}`} style={{ alignItems: 'flex-start' }}>
                <input type="radio" name="stopmode" checked={choice === o.id} onChange={() => { setChoice(o.id); setPhase('choose'); }} style={{ width: 20, height: 20, accentColor: 'var(--teal)', marginTop: 2 }} />
                <div className="grow">
                  <div className="bold row"><Icon name={o.icon} size={18} /> {o.title}</div>
                  <div className="small muted">{o.desc}</div>
                </div>
              </label>
            ))}
            {choice === 'send' && !s.internet && (
              <Banner tone="warn" icon="world-off">มือถือไม่มีอินเทอร์เน็ต แอปจะรับข้อมูลเก็บไว้ก่อน แล้วส่งขึ้น Server เมื่อมีอินเทอร์เน็ต</Banner>
            )}
            {err && <Banner tone="bad" title={ERR_TEXT[err]?.title || 'อุปกรณ์ไม่จบทริป'}>{ERR_TEXT[err]?.fix}</Banner>}
          </>
        )}
        {phase === 'confirm' && (
          <Banner tone="warn" title={`ยืนยัน${choice === 'send' ? 'จบทริปและส่งข้อมูล' : 'จบทริป'}?`}>
            หลังจบทริปแล้ว จะกลับไปบันทึกต่อในทริปเดิมไม่ได้
          </Banner>
        )}
        {phase === 'busy' && <Banner tone="info" icon="loader-2" title="กำลังสั่งอุปกรณ์หยุดบันทึก…">รอการยืนยันจากกล่อง</Banner>}
        {phase === 'unknown' && (
          <Banner tone="warn" title="ยังไม่ได้รับการยืนยันจากอุปกรณ์" actions={<Btn size="sm" kind="teal" icon="nfc" onClick={retry}>เชื่อมต่อแล้วลองอีกครั้ง</Btn>}>
            การเชื่อมต่อหลุดก่อนกล่องตอบกลับ ทริปอาจยังบันทึกอยู่ เชื่อมต่อใหม่เพื่อยืนยัน ระบบจะไม่จบทริปซ้ำหรือสร้างรายการใหม่
          </Banner>
        )}
        {phase === 'lost' && (
          <Banner tone="bad" title="การเชื่อมต่อหลุด" actions={<Btn size="sm" kind="teal" icon="nfc" onClick={retry}>เชื่อมต่อแล้วลองอีกครั้ง</Btn>}>
            อุปกรณ์ยังไม่ได้รับคำสั่ง ทริปยังบันทึกอยู่
          </Banner>
        )}
      </div>
      <div className="footer-bar stack tight">
        {phase === 'choose' && (
          <Btn block disabled={!linked} onClick={() => setPhase('confirm')}>
            ถัดไป
          </Btn>
        )}
        {phase === 'confirm' && (
          <>
            <Btn kind="danger" block icon="player-stop" disabled={!linked} onClick={doStop}>
              ยืนยัน{choice === 'send' ? 'จบทริปและส่งข้อมูล' : 'จบทริป'}
            </Btn>
            <Btn kind="secondary" block onClick={() => setPhase('choose')}>ยกเลิก</Btn>
          </>
        )}
        {phase === 'busy' && <Btn block busy>กำลังจบทริป…</Btn>}
      </div>
    </div>
  );
}
