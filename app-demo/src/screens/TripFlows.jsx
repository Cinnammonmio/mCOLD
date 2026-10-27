// Start trip lives on the trip screen itself (Trip.jsx: QuickStart); this file keeps its checks.
// Stop trip: summary → choose action → confirm → result.
import { useRef, useState } from 'react';
import { nav, toast, useApp, simNow, userName } from '../store.js';
import { BATT_TRIP_MV, BleLost, deviceView, findDevice, isLinked, pendingCommand, runBatch, stopTrip, tripSeries } from '../sim.js';
import { fmtDateTime, fmtDuration, fmtNum, fmtPct, fmtTemp, tripLabel } from '../format.js';
import { requestConnect } from '../ConnectSheet.jsx';
import { AppBar, Banner, Btn, Chip, Icon } from '../ui.jsx';
import { NeedConnection } from './Mcold.jsx';

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
