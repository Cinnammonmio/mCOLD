// Demo controls: kept apart from the user-facing app. Simulates link, internet, server and box faults.
import { useState } from 'react';
import { resetAll, update, useApp, simNow } from './store.js';
import { demoAdvance, demoToggleBattery, demoToggleHigh, demoToggleProbe, disconnect, findDevice, isLinked, setInternet } from './sim.js';
import { fmtDateTime } from './format.js';
import { Btn, Icon, Seg } from './ui.jsx';

const HOUR = 3600000;

export function DemoPanel({ onOpenGuide }) {
  const s = useApp();
  const dev = findDevice(s, s.selectedSn);
  const linkedDev = s.ble ? findDevice(s, s.ble.sn) : null;
  const [confirmReset, setConfirmReset] = useState(false);
  const setDemo = (k, v) => update((st) => void (st.demo[k] = v));
  const high = dev.episodes.some((e) => e.type === 'high' && e.to == null);
  const probe = dev.episodes.some((e) => e.type === 'probe' && e.to == null);

  return (
    <div className="demo-panel">
      <div className="demo-head">
        <div className="row"><Icon name="flask-2" /> <span className="t">ควบคุมเดโม</span></div>
        <p>โหมดจำลองทั้งหมด ไม่มีการเรียก Bluetooth, NFC, API หรือ Server จริง แผงนี้ไม่ใช่ส่วนหนึ่งของแอปผู้ใช้</p>
      </div>

      <div className="card">
        <h4><Icon name="device-mobile" size={15} /> อินเทอร์เน็ตของมือถือ</h4>
        <Seg demo label="อินเทอร์เน็ตของมือถือ" value={s.internet ? 'on' : 'off'} onChange={(v) => setInternet(v === 'on')} options={[{ value: 'on', label: 'มี' }, { value: 'off', label: 'ไม่มี' }]} />
      </div>

      <div className="card stack tight">
        <h4><Icon name="bluetooth" size={15} /> Bluetooth</h4>
        <div className="small">{linkedDev ? <>เชื่อมต่ออยู่กับ <b>{linkedDev.name}</b></> : 'ไม่ได้เชื่อมต่อกับเครื่องใด'}</div>
        <div className="row wrap">
          <Btn kind="demo" size="sm" icon="bluetooth-off" disabled={!s.ble} onClick={disconnect}>ทำให้หลุดตอนนี้</Btn>
        </div>
        <label className="check small">
          <input type="checkbox" checked={s.demo.bleDropNext} onChange={(e) => setDemo('bleDropNext', e.target.checked)} />
          <span>หลุดระหว่างคำสั่งถัดไป <span className="muted">(เริ่ม/จบทริป, รับข้อมูล, บันทึกค่า)</span></span>
        </label>
        <div className="tiny muted">ผลการแตะ NFC เลือกได้ในหน้าต่างเชื่อมต่อ</div>
        <Seg demo label="ผลการแตะ NFC ครั้งถัดไป" value={s.demo.nfcOutcome} onChange={(v) => setDemo('nfcOutcome', v)} options={[{ value: 'ok', label: 'สำเร็จ' }, { value: 'notfound', label: 'ไม่พบ' }, { value: 'connfail', label: 'ต่อไม่ได้' }, { value: 'authfail', label: 'สิทธิ์ไม่ผ่าน' }]} />
      </div>

      <div className="card stack tight">
        <h4><Icon name="server" size={15} /> Server</h4>
        <Seg demo label="ผลจาก Server" value={s.demo.serverMode} onChange={(v) => setDemo('serverMode', v)} options={[{ value: 'ok', label: 'รับสำเร็จ' }, { value: 'fail', label: 'ล้มเหลว' }]} />
        <label className="check small">
          <input type="checkbox" checked={s.demo.markFailNext} onChange={(e) => setDemo('markFailNext', e.target.checked)} />
          <span>Server รับแล้ว แต่แจ้งอุปกรณ์ไม่สำเร็จ (ครั้งถัดไป)</span>
        </label>
      </div>

      <div className="card stack tight">
        <h4><Icon name="adjustments-horizontal" size={15} /> ผลการบันทึกค่าตั้ง (ครั้งถัดไป)</h4>
        <Seg demo label="ผลการบันทึกค่าตั้ง" value={s.demo.settingsResult} onChange={(v) => setDemo('settingsResult', v)} options={[{ value: 'ok', label: 'สำเร็จ' }, { value: 'partial', label: 'บางค่า' }, { value: 'fail', label: 'ล้มเหลว' }]} />
      </div>

      <div className="card stack tight">
        <h4><Icon name="package" size={15} /> จำลองเหตุที่เครื่อง</h4>
        <select id="demo-dev" aria-label="เครื่องที่จำลอง" className="input" value={dev.sn} onChange={(e) => update((st) => void (st.selectedSn = e.target.value))}>
          {s.devices.map((d) => <option key={d.sn} value={d.sn}>{d.name}{s.ble?.sn === d.sn ? ' (เชื่อมต่ออยู่)' : ''}</option>)}
        </select>
        {!isLinked(s, dev.sn) && <div className="tiny muted">ไม่ได้เชื่อมต่อ: แอปจะเห็นผลเมื่อเชื่อมต่อครั้งถัดไป</div>}
        <div className="row wrap">
          <Btn kind="demo" size="sm" icon="temperature" onClick={() => demoToggleHigh(dev.sn)}>{high ? 'อุณหภูมิกลับปกติ' : 'อุณหภูมิเกินช่วง'}</Btn>
          <Btn kind="demo" size="sm" icon="alert-triangle" onClick={() => demoToggleProbe(dev.sn)}>{probe ? 'หัววัดกลับมา' : 'ไม่พบหัววัด'}</Btn>
          <Btn kind="demo" size="sm" icon="battery-1" onClick={() => demoToggleBattery(dev.sn)}>{dev.lowBattery ? 'แบตเตอรี่ปกติ' : 'แบตเตอรี่ต่ำ'}</Btn>
        </div>
        <div className="tiny muted">แจ้งเตือนอุณหภูมิในเดโมขึ้นทันที โดยข้ามเวลาหน่วงของอุปกรณ์</div>
      </div>

      <div className="card stack tight">
        <h4><Icon name="clock" size={15} /> เวลาจำลอง</h4>
        <div className="small num">{fmtDateTime(simNow())}</div>
        <div className="row wrap">
          <Btn kind="demo" size="sm" onClick={() => demoAdvance(0.5 * HOUR)}>+30 นาที</Btn>
          <Btn kind="demo" size="sm" onClick={() => demoAdvance(6 * HOUR)}>+6 ชม.</Btn>
        </div>
      </div>

      <div className="card stack tight">
        <h4><Icon name="refresh" size={15} /> ข้อมูลจำลอง</h4>
        {!confirmReset ? (
          <Btn kind="demo" size="sm" icon="trash" onClick={() => setConfirmReset(true)}>รีเซ็ตข้อมูลทั้งหมด</Btn>
        ) : (
          <div className="stack tight">
            <div className="small">ล้างทริป ค่าตั้ง และคิวในแอป กลับเป็นข้อมูลเริ่มต้น?</div>
            <div className="row">
              <Btn kind="danger" size="sm" onClick={() => { resetAll(); setConfirmReset(false); }}>รีเซ็ต</Btn>
              <Btn kind="secondary" size="sm" onClick={() => setConfirmReset(false)}>ยกเลิก</Btn>
            </div>
          </div>
        )}
      </div>

      <Btn kind="secondary" icon="book-2" onClick={onOpenGuide}>คู่มือสำหรับทีมพัฒนา</Btn>
    </div>
  );
}
