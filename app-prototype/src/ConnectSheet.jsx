// Simulated "tap NFC → find box → BLE → authorize → ready" (ble-protocol.md §1–2).
// No pairing code: the tap itself proves the phone is next to the box.
import { useEffect, useRef, useState } from 'react';
import { getState, update, updateUi, useApp } from './store.js';
import { finishConnect, findDevice } from './sim.js';
import { Btn, Icon, Seg, Sheet, Steps } from './ui.jsx';

const wait = (ms) => new Promise((r) => setTimeout(r, ms));

/** Open the connect sheet for a box. Resolves true once connected and authorized. */
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
  const dev = findDevice(s, req.sn);
  const [phase, setPhase] = useState('idle'); // idle | running | error | done
  const [at, setAt] = useState(-1);
  const [fail, setFail] = useState(null);
  const alive = useRef(true);
  useEffect(() => () => void (alive.current = false), []);

  const close = (ok) => {
    updateUi(() => ({ connect: null }));
    req.resolve(ok);
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
    finishConnect(req.sn);
    setAt(STEPS.length - 1);
    setPhase('done');
    await wait(700);
    if (alive.current) close(true);
  };

  const items = STEPS.map((st, i) => {
    let state = 'todo';
    if (phase === 'done' || i < at) state = 'done';
    else if (i === at) state = phase === 'error' && fail?.at === i ? 'error' : 'active';
    if (phase === 'done' && i === STEPS.length - 1) state = 'done';
    const sub = st.key === 'find' && state === 'done' ? `${dev.name} · ${dev.sn}` : st.sub;
    return { ...st, sub, state };
  });

  const other = s.ble && s.ble.sn !== req.sn ? findDevice(s, s.ble.sn) : null;

  return (
    <Sheet onClose={() => phase !== 'running' && close(false)} label="เชื่อมต่ออุปกรณ์">
      <div className="stack">
        <div>
          <h2>เชื่อมต่อ {dev.name}</h2>
          <div className="small muted">หมายเลขเครื่อง {dev.sn}</div>
        </div>
        {req.reason && <div className="small">{req.reason}</div>}

        <div className={`nfc-art ${phase === 'running' && at === 0 ? 'tapping' : ''}`}>
          <div className="box">
            <div className={`tag ${phase === 'idle' ? 'pulse' : ''}`} style={{ position: 'relative' }}>
              <Icon name="nfc" size={20} />
            </div>
          </div>
          <div className="phone-ic" />
        </div>

        {phase === 'idle' && (
          <>
            <p className="center small" style={{ margin: 0 }}>
              นำด้านหลังของมือถือแตะที่สัญลักษณ์ <b>NFC</b> บนกล่อง ค้างไว้จนกว่าจะเชื่อมต่อเสร็จ
            </p>
            {other && <div className="tiny muted center">ระบบจะตัดการเชื่อมต่อกับ {other.name} ก่อน</div>}
          </>
        )}

        {phase !== 'idle' && <Steps items={items} />}

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
            <DemoOutcome />
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

function DemoOutcome() {
  const s = useApp();
  return (
    <div className="stack tight" style={{ border: '1px dashed #cfc2f0', borderRadius: 12, padding: 10, background: 'var(--demo-bg)' }}>
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
