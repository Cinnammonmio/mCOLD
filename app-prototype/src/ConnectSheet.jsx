// Simulated "tap NFC → find box → BLE → authorize → ready" (ble-protocol.md §1–2).
// No pairing code: the tap itself proves the phone is next to the box.
import { Fragment, useEffect, useRef, useState } from 'react';
import { getState, update, updateUi, useApp } from './store.js';
import { deviceView, finishConnect, findDevice, pendingTrips } from './sim.js';
import { fmtPct, fmtTemp, tripLabel } from './format.js';
import { Btn, Icon, Seg, Sheet, Steps } from './ui.jsx';

const wait = (ms) => new Promise((r) => setTimeout(r, ms));

/**
 * Open the connect sheet. `sn` = the box we expect; null = whichever box the phone touches.
 * Resolves with the connected SN (truthy) once connected and authorized, or false.
 */
export function requestConnect(sn, reason) {
  return new Promise((resolve) => {
    updateUi(() => ({ connect: { sn, reason, resolve } }));
  });
}

const STEPS = [
  { key: 'tap', title: 'แตะ NFC', sub: 'อ่านข้อมูลจากสัญลักษณ์ NFC บนกล่อง' },
  { key: 'find', title: 'พบอุปกรณ์', sub: 'ค้นหากล่องที่ตรงกับหมายเลขเครื่อง' },
  { key: 'ble', title: 'เชื่อมต่อ Bluetooth', sub: 'เชื่อมต่อโดยไม่ต้องจับคู่หรือกรอกรหัส' },
  { key: 'auth', title: 'ยืนยันสิทธิ์', sub: 'ยืนยันว่ามือถืออยู่ใกล้กล่องจริง' },
  { key: 'ready', title: 'พร้อมสั่งงาน', sub: '' },
];

const FAIL = {
  notfound: {
    at: 1,
    title: 'ไม่พบอุปกรณ์',
    body: 'ไม่พบสัญญาณจากกล่องนี้ วางมือถือแนบสัญลักษณ์ NFC บนกล่องค้างไว้ 1–2 วินาที แล้วแตะอีกครั้ง หากหน้าจอกล่องดับ ให้ตรวจว่าเปิดเครื่องอยู่',
  },
  connfail: {
    at: 2,
    title: 'เชื่อมต่อไม่สำเร็จ',
    body: 'กล่องเปิดรับการเชื่อมต่อประมาณ 1 นาทีหลังแตะ แตะ NFC อีกครั้งเพื่อเปิดรับใหม่ และถือมือถือไว้ใกล้กล่อง',
  },
  authfail: {
    at: 3,
    title: 'ยืนยันสิทธิ์ไม่ผ่าน',
    body: 'รหัสจากการแตะครั้งก่อนหมดอายุแล้ว แตะ NFC อีกครั้งเพื่อรับรหัสล่าสุด',
  },
};

export function ConnectSheet() {
  const s = useApp();
  const req = s.ui.connect;
  if (!req) return null;
  return <ConnectFlow key={req.sn + (req.reason || '')} req={req} />;
}

function ConnectFlow({ req }) {
  const s = useApp();
  // Without an expected box the tag tells us which one it is; the demo lets you pick that box.
  const [picked, setPicked] = useState(req.sn || s.selectedSn);
  const dev = findDevice(s, req.sn || picked);
  const [phase, setPhase] = useState('idle'); // idle | running | error | done
  const [at, setAt] = useState(-1);
  const [fail, setFail] = useState(null);
  const alive = useRef(true);
  useEffect(() => () => void (alive.current = false), []);

  const close = (ok) => {
    updateUi(() => ({ connect: null }));
    req.resolve(ok ? dev.sn : false);
  };

  const run = async () => {
    const outcome = getState().demo.nfcOutcome;
    if (outcome !== 'ok') update((st) => void (st.demo.nfcOutcome = 'ok')); // one-shot fault
    const f = FAIL[outcome];
    setFail(null);
    setPhase('running');
    for (let i = 0; i < STEPS.length - 1; i++) {
      setAt(i);
      await wait(i === 0 ? 900 : 650);
      if (!alive.current) return;
      if (f && f.at === i) {
        setFail({ ...f, at: i });
        setPhase('error');
        return;
      }
    }
    finishConnect(dev.sn);
    setAt(STEPS.length - 1);
    setPhase('done'); // stay open so the user sees what the box reported
  };

  const items = STEPS.map((st, i) => {
    let state = 'todo';
    if (phase === 'done' || i < at) state = 'done';
    else if (i === at) state = phase === 'error' && fail?.at === i ? 'error' : 'active';
    if (phase === 'done' && i === STEPS.length - 1) state = 'done';
    // What the app has in hand after each step (ble-protocol.md §1–2). The tag key is never shown.
    const got = {
      tap: `หมายเลขเครื่อง ${dev.sn} · Bluetooth ${dev.bleMac} · รหัสยืนยัน: ได้รับแล้ว (ไม่แสดง)`,
      find: `พบอุปกรณ์ชื่อ ${dev.sn} ในระยะ`,
      ble: `Firmware ${dev.fw} · ฮาร์ดแวร์ ${dev.hw} · โปรโตคอล ${dev.proto}`,
      auth: 'ยืนยันแล้ว สั่งงานได้จนกว่าจะตัดการเชื่อมต่อ',
      ready: 'ได้รับสถานะและค่าตั้งของอุปกรณ์แล้ว',
    };
    const sub = state === 'done' ? got[st.key] : st.sub;
    return { ...st, sub, state };
  });

  const other = s.ble && s.ble.sn !== dev.sn ? findDevice(s, s.ble.sn) : null;

  return (
    <Sheet onClose={() => phase !== 'running' && close(false)} label="เชื่อมต่ออุปกรณ์">
      <div className="stack">
        <div>
          <h2>{req.sn ? `เชื่อมต่อ ${dev.name}` : 'เชื่อมต่ออุปกรณ์ mCOLD'}</h2>
          {req.sn && <div className="small muted">หมายเลขเครื่อง {dev.sn}</div>}
        </div>
        {req.reason && <div className="small">{req.reason}</div>}

        {phase !== 'done' && (
          <div className={`nfc-art ${phase === 'running' && at === 0 ? 'tapping' : ''}`}>
            <div className="box">
              <div className={`tag ${phase === 'idle' ? 'pulse' : ''}`} style={{ position: 'relative' }}>
                <Icon name="nfc" size={20} />
              </div>
            </div>
            <div className="phone-ic" />
          </div>
        )}

        {phase === 'idle' && (
          <>
            <p className="center small" style={{ margin: 0 }}>
              นำด้านหลังของมือถือแตะที่สัญลักษณ์ <b>NFC</b> บนกล่อง ค้างไว้จนกว่าจะเชื่อมต่อเสร็จ
            </p>
            {other && <div className="tiny muted center">ระบบจะตัดการเชื่อมต่อกับ {other.name} ก่อน</div>}
          </>
        )}

        {phase !== 'idle' && <Steps items={items} />}
        {phase === 'done' && (
          <>
            <DeviceReadout sn={dev.sn} />
            <Btn kind="teal" block icon="check" onClick={() => close(true)}>
              ดำเนินการต่อ
            </Btn>
          </>
        )}

        {phase === 'error' && (
          <div className="banner bad" role="alert">
            <Icon name="alert-circle" />
            <div>
              <b>{fail.title}</b>
              <div>{fail.body}</div>
            </div>
          </div>
        )}

        {phase === 'idle' && (
          <>
            <Btn kind="teal" block icon="nfc" onClick={run}>
              จำลองแตะ NFC
            </Btn>
            <DemoOutcome picker={req.sn ? null : { value: picked, onChange: setPicked }} />
          </>
        )}
        {phase === 'error' && (
          <>
            <Btn kind="teal" block icon="refresh" onClick={run}>
              แตะ NFC อีกครั้ง
            </Btn>
            <DemoOutcome />
          </>
        )}
        {phase !== 'running' && phase !== 'done' && (
          <Btn kind="secondary" block onClick={() => close(false)}>
            ยกเลิก
          </Btn>
        )}
      </div>
    </Sheet>
  );
}

function DemoOutcome({ picker }) {
  const s = useApp();
  return (
    <div className="stack tight" style={{ border: '1px dashed #cfc2f0', borderRadius: 12, padding: 10, background: 'var(--demo-bg)' }}>
      {picker && (
        <>
          <label htmlFor="demo-tapped" className="tiny bold" style={{ color: 'var(--demo)' }}>
            <Icon name="flask-2" size={13} /> โหมดจำลอง · เครื่องที่แตะ
          </label>
          <select id="demo-tapped" className="input" value={picker.value} onChange={(e) => picker.onChange(e.target.value)}>
            {s.devices.map((d) => (
              <option key={d.sn} value={d.sn}>{d.name} · {d.sn}</option>
            ))}
          </select>
        </>
      )}
      <div className="tiny bold" style={{ color: 'var(--demo)' }}>
        <Icon name="flask-2" size={13} /> โหมดจำลอง · ผลของการแตะครั้งนี้
      </div>
      <Seg
        demo
        label="ผลการแตะจำลอง"
        value={s.demo.nfcOutcome}
        onChange={(v) => update((st) => void (st.demo.nfcOutcome = v))}
        options={[
          { value: 'ok', label: 'สำเร็จ' },
          { value: 'notfound', label: 'ไม่พบ' },
          { value: 'connfail', label: 'ต่อไม่ได้' },
          { value: 'authfail', label: 'สิทธิ์ไม่ผ่าน' },
        ]}
      />
    </div>
  );
}

const TIME_SRC = { ntp: 'ตั้งจากอินเทอร์เน็ต', gnss: 'ตั้งจากดาวเทียม', host: 'ตั้งจากมือถือ', rtc: 'นาฬิกาในเครื่อง', none: 'ยังไม่มีเวลา' };
const ALARM_TH = { ALARM_HIGH: 'อุณหภูมิสูง', ALARM_LOW: 'อุณหภูมิต่ำ', ALARM_PROBE: 'หัววัด', BATTERY_LOW: 'แบตต่ำ' };

/** What the box reports right after connecting (STATUS + WIFI event), in plain words. */
export function DeviceReadout({ sn }) {
  const s = useApp();
  const dev = findDevice(s, sn);
  const v = deviceView(s, dev);
  const pending = pendingTrips(s, dev).length;
  const w = v.wifiNow || {};
  const rows = [
    ['อุณหภูมิ', v.known ? (v.temp == null ? 'ไม่พบหัววัด' : fmtTemp(v.temp, 2)) : '—'],
    ['แบตเตอรี่', v.battery == null ? '—' : `${fmtPct(v.battery)} · ${v.batteryMv ?? '—'} mV · ${v.external ? 'เสียบไฟอยู่' : 'ใช้แบตเตอรี่'}`],
    ['ทริป', v.trip ? `${tripLabel(v.trip)} · กำลังบันทึก` : 'ไม่มีทริปทำงาน'],
    ['แจ้งเตือนค้าง', v.trip ? (v.trip.alarms.active.length ? v.trip.alarms.active.map((a) => ALARM_TH[a]).join(', ') : `ไม่มี (ทั้งทริป ${v.trip.alarms.raised} ครั้ง)`) : 'ไม่มี'],
    ['ทริปรอส่ง', `${pending} ทริป`],
    ['เวลาของเครื่อง', TIME_SRC[v.timeQuality] || '—'],
    ['ตำแหน่ง', v.gnss.state === 'none' ? 'ยังไม่พบตำแหน่ง' : `${v.gnss.lat.toFixed(4)}, ${v.gnss.lon.toFixed(4)}`],
    ['พื้นที่บันทึก', `ใช้ไป ${fmtPct(v.storagePct)}`],
    ['Wi-Fi ของเครื่อง', w.connected ? `${w.ssid} · ${w.rssi} dBm` : 'ไม่ได้เชื่อมต่อ'],
  ];
  return (
    <div className="card stack tight" style={{ background: 'var(--soft)', borderColor: 'transparent' }}>
      <h3 className="row"><Icon name="device-mobile" size={18} className="muted" /> ข้อมูลที่ได้รับจากอุปกรณ์</h3>
      <dl className="kv">
        {rows.map(([k, val]) => (
          <Fragment key={k}>
            <dt>{k}</dt>
            <dd>{val}</dd>
          </Fragment>
        ))}
      </dl>
    </div>
  );
}
