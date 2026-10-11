// Device settings: show current values → edit → save/cancel → per-value result.
// Values and ranges follow docs/app/device-settings.md. Wi-Fi uses its own per-slot commands.
import { useMemo, useState } from 'react';
import { DEFAULT_CONFIG, nav, toast, useApp, TRIP_LOCKED, userName } from '../store.js';
import { applyConfig, BleLost, CONFIG_RULES, deviceView, findDevice, isLinked, wifiCommand } from '../sim.js';
import { fmtPeriod, fmtTime, fmtTz } from '../format.js';
import { requestConnect } from '../ConnectSheet.jsx';
import { AppBar, Banner, Btn, Chip, Field, Icon, Seg, Switch } from '../ui.jsx';
import { DeviceContext, NeedConnection } from './Mcold.jsx';

// Every value the app may set, with the default, range and meaning from docs/app/device-settings.md.
// UI units are converted here (seconds → minutes, 0.01 °C → °C …); the box always gets its own units.
const num = (label, unit, factor, min, max, step, help) => ({ label, unit, toUi: (v) => v / factor, toDev: (u) => Math.round(u * factor), min, max, step, help });
const FIELDS = {
  sample_period_s: num('รอบบันทึกอุณหภูมิ', 'นาที', 60, 1, 60, 1, 'ค่าเริ่มต้น 5 นาที · ตั้งได้ 1–60 นาที'),
  temp_adj_c100: num('ปรับค่าอุณหภูมิ', '°C', 100, -10, 10, 0.01, 'บวกเพิ่มจากค่าที่วัด ใช้ทั้งบนจอ ในข้อมูลทริป และที่ส่ง Server · ค่าเริ่มต้น 0 · −10.00 ถึง +10.00°C'),
  alarm_dwell_s: num('เวลานอกช่วงก่อนแจ้งเตือน', 'นาที', 60, 0, 60, 1, 'อุณหภูมิต้องอยู่นอกช่วงต่อเนื่องนานเท่านี้ก่อนเตือน 0 = ทันที · ค่าเริ่มต้น 5 นาที · 0–60 นาที'),
  alarm_hyst_c10: num('ระยะกลับเข้าช่วง (Hysteresis)', '°C', 10, 0, 10, 0.1, 'ต้องกลับเข้าในช่วงเกินเส้นเท่านี้ก่อนถือว่าการเตือนจบ กันเตือนสลับถี่เมื่อค่าแกว่งตรงเส้น · ค่าเริ่มต้น 0.5°C · 0–10.0°C'),
  gnss_period_s: num('รอบหาตำแหน่ง GPS ระหว่างทริป', 'นาที', 60, 5, 1440, 1, 'ค่าเริ่มต้น 30 นาที · 5 นาที ถึง 24 ชม. (1440 นาที)'),
  tz_offset_min: {
    label: 'เขตเวลา',
    kind: 'select',
    options: [-720, -600, -480, -300, -180, 0, 60, 120, 180, 240, 330, 360, 420, 480, 540, 600, 720, 840].map((m) => ({ value: m, label: fmtTz(m) + (m === 420 ? ' (ประเทศไทย)' : '') })),
    help: 'ค่าเริ่มต้น UTC+7 · ตั้งได้ UTC−12 ถึง UTC+14',
  },
  buzzer_enabled: { label: 'เสียงเตือนที่กล่อง', kind: 'switch', help: 'ค่าเริ่มต้น เปิด' },
  led_front_pct: num('ความสว่างไฟหน้า 3 ดวง', '%', 1, 1, 100, 1, 'ค่าเริ่มต้น 2% · 1–100%'),
  led_bright_pct: num('ความสว่างไฟข้าง (ชาร์จ)', '%', 1, 1, 100, 1, 'ค่าเริ่มต้น 20% · 1–100%'),
  led_status_s: num('รอบกะพริบบอกสถานะตอนใช้แบต', 'นาที', 60, 1, 60, 1, 'ค่าเริ่มต้น 15 นาที · 1–60 นาที'),
  idle_wake_s: num('ตื่นตรวจสถานะอย่างน้อยทุก', 'นาที', 60, 5, 1440, 1, 'ตอนไม่มีอะไรทำ · ค่าเริ่มต้น 60 นาที · 5 นาที ถึง 24 ชม.'),
  accel_wake_ths: num('ความไวการจับการขยับ', 'ระดับ', 1, 1, 63, 1, 'ระดับละ 31 mg ยิ่งน้อยยิ่งไว · ค่าเริ่มต้น 2 (62 mg) · 1–63'),
  upload_period_s: num('รอบส่งข้อมูลผ่าน Wi-Fi ของอุปกรณ์', 'นาที', 60, 1, 1440, 1, 'รอบที่กล่องเปิด Wi-Fi ของตัวเองส่งสถานะตอนใช้แบต ยิ่งถี่ยิ่งกินแบต · ค่าเริ่มต้น 5 นาที · 1 นาที ถึง 24 ชม.'),
  usb_drive: { label: 'เสียบ USB แล้วเป็นไดรฟ์', kind: 'switch', help: 'ค่าเริ่มต้น เปิด · ปิดแล้วเครื่องรีสตาร์ทกลับเป็นพอร์ตแบบเดิม ใช้เป็นทางกู้ถ้าไดรฟ์มีปัญหา' },
};

const GROUPS = [
  { id: 'log', title: 'การบันทึกและอุณหภูมิ', icon: 'temperature', keys: ['sample_period_s', 'temp_adj_c100'] },
  { id: 'alarm', title: 'การแจ้งเตือน', icon: 'bell-ringing', keys: ['alarm_dwell_s', 'alarm_hyst_c10'] },
  { id: 'loc', title: 'ตำแหน่งและเวลา', icon: 'map-pin', keys: ['gnss_period_s', 'tz_offset_min'] },
  { id: 'led', title: 'ไฟและเสียง', icon: 'sun', keys: ['buzzer_enabled', 'led_front_pct', 'led_bright_pct', 'led_status_s'] },
  { id: 'power', title: 'พลังงานและการขยับ', icon: 'battery-3', keys: ['idle_wake_s', 'accel_wake_ths'] },
];
const ADV_KEYS = ['upload_period_s', 'usb_drive'];

function uiValue(key, devVal) {
  const f = FIELDS[key];
  if (f.kind === 'select') return devVal;
  if (f.kind === 'switch') return !!devVal;
  const u = f.toUi(devVal);
  return f.step < 1 ? u.toFixed(f.step === 0.01 ? 2 : 1) : String(u);
}

function validate(key, raw) {
  const f = FIELDS[key];
  if (f.kind) return null;
  const n = Number(String(raw).replace(',', '.'));
  if (String(raw).trim() === '' || !Number.isFinite(n)) return 'กรอกตัวเลข';
  if (n < f.min || n > f.max) return `ตั้งได้ ${f.min} ถึง ${f.max} ${f.unit}`;
  const digits = f.step === 0.01 ? 2 : f.step === 0.1 ? 1 : 0;
  if (Math.abs(n * 10 ** digits - Math.round(n * 10 ** digits)) > 1e-6) return digits ? `ทศนิยมไม่เกิน ${digits} ตำแหน่ง` : 'ใช้จำนวนเต็ม';
  return null;
}

function toDevice(key, raw) {
  const f = FIELDS[key];
  if (f.kind === 'select') return Number(raw);
  if (f.kind === 'switch') return raw ? 1 : 0;
  return f.toDev(Number(String(raw).replace(',', '.')));
}

function displayValue(key, devVal) {
  const f = FIELDS[key];
  if (f.kind === 'select') return f.options.find((o) => o.value === devVal)?.label || fmtTz(devVal);
  if (f.kind === 'switch') return devVal ? 'เปิด' : 'ปิด';
  if (key === 'temp_adj_c100') return `${devVal >= 0 ? '+' : '−'}${Math.abs(devVal / 100).toFixed(2)} °C`;
  if (key === 'alarm_dwell_s' && devVal === 0) return 'ทันที';
  return `${uiValue(key, devVal)} ${f.unit}`;
}

export function SettingsTab() {
  const s = useApp();
  const dev = findDevice(s, s.selectedSn);
  const v = deviceView(s, dev);
  const linked = isLinked(s, dev.sn);
  const tripOn = !!dev.activeTripId && v.known;
  const [editing, setEditing] = useState(false);
  const [draft, setDraft] = useState({});
  const [busy, setBusy] = useState(false);
  const [result, setResult] = useState(null); // { applied, errors } | { lost: true }
  const [showAdv, setShowAdv] = useState(false);

  const keys = useMemo(() => [...GROUPS.flatMap((g) => g.keys), ...ADV_KEYS], []);
  const startEdit = () => {
    const d = {};
    keys.forEach((k) => (d[k] = uiValue(k, dev.config[k])));
    setDraft(d);
    setResult(null);
    setEditing(true);
  };
  const errors = {};
  if (editing) keys.forEach((k) => { const e = validate(k, draft[k]); if (e) errors[k] = e; });
  const changes = {};
  if (editing) keys.forEach((k) => { if (!errors[k] && toDevice(k, draft[k]) !== dev.config[k]) changes[k] = toDevice(k, draft[k]); });
  const nChanged = Object.keys(changes).length;

  const save = async () => {
    if (busy || nChanged === 0 || Object.keys(errors).length) return;
    setBusy(true);
    setResult(null);
    try {
      const res = await applyConfig(dev.sn, changes);
      setResult(res);
      const nErr = Object.keys(res.errors).length;
      if (nErr === 0) {
        toast(`บันทึกค่าในอุปกรณ์แล้ว ${res.applied.length} ค่า`, 'ok');
        setEditing(false);
      }
      // On a partial result the form stays open: applied values now match the box, so only the failed ones remain as changes.
    } catch (e) {
      if (e instanceof BleLost) setResult({ lost: true });
      else throw e;
    } finally {
      setBusy(false);
    }
  };

  const rowResult = (k) => {
    if (!result || result.lost) return null;
    if (result.applied.includes(k)) return <div className="result-ok"><Icon name="check" size={14} stroke={2.5} /> บันทึกแล้ว</div>;
    if (result.errors[k]) return <div className="result-bad"><Icon name="x" size={14} stroke={2.5} /> ยังไม่ได้บันทึก: {result.errors[k]}</div>;
    return null;
  };

  return (
    <>
      <DeviceContext lockedReason={editing ? 'บันทึกหรือยกเลิกการแก้ไขก่อนเปลี่ยนเครื่อง' : null} />
      {!v.known && <NeedConnection dev={dev} text="ยังไม่เคยอ่านค่าจากเครื่องนี้ เชื่อมต่อเพื่อดูค่าปัจจุบัน" />}
      {v.known && (
        <>
          {!linked && (
            <Banner tone="neutral" icon="history" title={`ค่าที่อ่านล่าสุดเมื่อ ${fmtTime(v.at)}`} actions={<Btn size="sm" kind="teal" icon="nfc" onClick={() => requestConnect(dev.sn, 'เชื่อมต่อเพื่ออ่านค่าล่าสุดและแก้ไข')}>เชื่อมต่อเพื่อแก้ไข</Btn>}>
              ต้องเชื่อมต่อก่อนจึงจะแก้ไขค่าได้
            </Banner>
          )}
          {linked && <div className="freshness live">ค่าปัจจุบันที่อุปกรณ์ส่งมาเมื่อเชื่อมต่อ ค่าเริ่มต้นและช่วงตามเอกสารค่าตั้งของเครื่อง</div>}
          {dev.lastConfig && <div className="freshness">แก้ไขล่าสุดจากแอปโดย {userName(s, dev.lastConfig.by)} · {fmtTime(dev.lastConfig.t)}</div>}
          {tripOn && (
            <Banner tone="warn" icon="lock" title="มีทริปกำลังทำงาน">
              ค่าที่ใช้กับข้อมูลทริปถูกล็อก แก้ไขได้หลังจบทริป ค่าอื่นแก้ไขได้ตามปกติ
            </Banner>
          )}
          {result?.lost && (
            <Banner tone="bad" title="ไม่ได้รับผลจากอุปกรณ์" actions={<Btn size="sm" kind="teal" icon="nfc" onClick={() => requestConnect(dev.sn, 'เชื่อมต่อใหม่แล้วบันทึกอีกครั้ง')}>เชื่อมต่อใหม่</Btn>}>
              การเชื่อมต่อหลุดระหว่างบันทึก ค่าอาจยังไม่ถูกบันทึก เชื่อมต่อใหม่แล้วกดบันทึกอีกครั้ง ค่าที่แก้ยังอยู่ในฟอร์ม
            </Banner>
          )}
          {result && !result.lost && Object.keys(result.errors).length > 0 && (
            <Banner tone={result.applied.length ? 'warn' : 'bad'} title={result.applied.length ? `บันทึกแล้ว ${result.applied.length} จาก ${result.applied.length + Object.keys(result.errors).length} ค่า` : 'บันทึกไม่สำเร็จ'}>
              ค่าที่ยังไม่ได้บันทึก: {Object.keys(result.errors).map((k) => FIELDS[k].label).join(', ')} ดูเหตุผลใต้แต่ละช่อง แล้วกดบันทึกอีกครั้ง
            </Banner>
          )}

          {GROUPS.map((g) => (
            <div key={g.id} className="card stack">
              <h3 className="row"><Icon name={g.icon} size={18} className="muted" /> {g.title}</h3>
              {g.keys.map((k) => (
                <SettingRow key={k} k={k} dev={dev} editing={editing} locked={tripOn && TRIP_LOCKED.includes(k)} draft={draft} setDraft={setDraft} error={errors[k]} result={rowResult(k)} />
              ))}
            </div>
          ))}

          <div className="card stack">
            <button className="row between" style={{ border: 0, background: 'none', padding: 0, width: '100%' }} onClick={() => setShowAdv(!showAdv)} aria-expanded={showAdv}>
              <h3 className="row"><Icon name="wifi" size={18} className="muted" /> เครือข่ายขั้นสูง</h3>
              <Icon name="chevron-down" style={{ transform: showAdv ? 'rotate(180deg)' : 'none' }} />
            </button>
            {!showAdv && <div className="small muted">Wi-Fi ของอุปกรณ์ รอบส่งผ่าน Wi-Fi และ USB</div>}
            {showAdv && (
              <>
                {ADV_KEYS.map((k) => (
                  <SettingRow key={k} k={k} dev={dev} editing={editing} locked={false} draft={draft} setDraft={setDraft} error={errors[k]} result={rowResult(k)} />
                ))}
                <Banner tone="info">
                  การส่งข้อมูลทริปผ่านมือถือทำที่เมนู <b>ส่งข้อมูล</b> และไม่ใช้รอบส่งของ Wi-Fi
                </Banner>
                <WifiNow dev={dev} />
                <WifiList dev={dev} linked={linked} />
                <dl className="kv">
                  <dt>ที่อยู่สำหรับอัปเดต Firmware</dt>
                  <dd>{dev.otaBase || '—'}</dd>
                </dl>
                <div className="tiny muted">Server ปลายทางของอุปกรณ์ (MQTT) และค่าโรงงานตั้งได้จากสาย USB เท่านั้น ไม่แก้จากแอป</div>
              </>
            )}
          </div>

          <div className="card stack tight">
            <h3 className="row"><Icon name="cpu" size={18} className="muted" /> ข้อมูลอุปกรณ์</h3>
            <dl className="kv">
              <dt>ชื่อในแอป</dt><dd>{dev.name}</dd>
              <dt>หมายเลขเครื่อง</dt><dd className="mono">{dev.sn}</dd>
              <dt>Firmware</dt><dd className="mono">{dev.fw}</dd>
              <dt>ฮาร์ดแวร์</dt><dd className="mono">{dev.hw}</dd>
              <dt>เวอร์ชันโปรโตคอล</dt><dd className="mono">{dev.proto}</dd>
              <dt>Bluetooth MAC</dt><dd className="mono">{dev.bleMac}</dd>
              <dt>Wi-Fi MAC</dt><dd className="mono">{dev.wifiMac}</dd>
              <dt>ค่าตั้งจาก Server ฉบับที่</dt><dd className="num">{dev.serverRev ?? '—'}</dd>
              <dt>เคาะเป็นคำสั่ง (ทดสอบ)</dt><dd>{dev.config.tap_test ? 'เปิด' : 'ปิด'}</dd>
              <dt>แสดงสิ่งที่รับบนจอ (ทดสอบ)</dt><dd>{dev.config.rx_show ? 'เปิด' : 'ปิด'}</dd>
            </dl>
            <div className="tiny muted">อ่านอย่างเดียว · ค่าทดสอบแก้ได้จากสาย USB เท่านั้น และไม่ใช้ในงานผลิต</div>
          </div>
          <div style={{ height: editing ? 70 : 0 }} />
        </>
      )}

      {v.known && (
        <div style={{ position: 'sticky', bottom: -24, padding: '10px 0 14px', background: 'linear-gradient(to top, var(--bg) 75%, transparent)', marginTop: editing ? -70 : 0 }}>
          {!editing ? (
            <Btn block icon="pencil" disabled={!linked} onClick={startEdit}>
              {linked ? 'แก้ไขค่า' : 'เชื่อมต่อก่อนแก้ไข'}
            </Btn>
          ) : (
            <div className="row">
              <Btn kind="secondary" className="grow" disabled={busy} onClick={() => { setEditing(false); setResult(null); }}>
                ยกเลิก
              </Btn>
              <Btn kind="teal" className="grow" busy={busy} disabled={nChanged === 0 || Object.keys(errors).length > 0 || !linked} onClick={save}>
                {busy ? 'กำลังบันทึก…' : nChanged ? `บันทึก ${nChanged} ค่า` : 'ยังไม่มีการแก้ไข'}
              </Btn>
            </div>
          )}
        </div>
      )}
    </>
  );
}

function SettingRow({ k, dev, editing, locked, draft, setDraft, error, result }) {
  const f = FIELDS[k];
  const id = `cfg-${k}`;
  const cur = dev.config[k];
  const changed = editing && !error && toDevice(k, draft[k]) !== cur;
  if (!editing || locked) {
    return (
      <div className="stack tight" style={{ gap: 2 }}>
        <div className="row between" style={{ alignItems: 'flex-start' }}>
          <span className="small" style={{ fontWeight: 500 }}>{f.label}</span>
          <span className="bold num" style={{ whiteSpace: 'nowrap' }}>{displayValue(k, cur)}</span>
        </div>
        {cur !== DEFAULT_CONFIG[k] && <div className="tiny muted">ค่าเริ่มต้น {displayValue(k, DEFAULT_CONFIG[k])}</div>}
        {locked && <div className="lockline"><Icon name="lock" size={14} /> แก้ไขได้หลังจบทริป</div>}
        {result}
      </div>
    );
  }
  if (f.kind === 'switch') {
    return (
      <div className="stack tight">
        <div className="row between">
          <label htmlFor={id} className="small" style={{ fontWeight: 600 }}>{f.label}</label>
          <Switch id={id} label={f.label} checked={draft[k]} onChange={(val) => setDraft({ ...draft, [k]: val })} />
        </div>
        {f.help && <div className="tiny muted">{f.help}</div>}
        {result}
      </div>
    );
  }
  if (f.kind === 'select') {
    return (
      <div className="stack tight">
        <Field label={f.label} htmlFor={id} help={f.help}>
          <select id={id} className="input" value={draft[k]} onChange={(e) => setDraft({ ...draft, [k]: Number(e.target.value) })}>
            {f.options.map((o) => <option key={o.value} value={o.value}>{o.label}{o.value === dev.config[k] ? ' (ปัจจุบัน)' : ''}</option>)}
          </select>
        </Field>
        {result}
      </div>
    );
  }
  return (
    <div className="stack tight">
      <Field label={f.label} htmlFor={id} error={error} help={f.help}>
        <div className="input-unit">
          <input id={id} className={`input num ${error ? 'invalid' : ''}`} inputMode="decimal" value={draft[k]} onChange={(e) => setDraft({ ...draft, [k]: e.target.value })} />
          <span className="unit">{f.unit}</span>
        </div>
      </Field>
      <div className="row between tiny muted">
        <span>ปัจจุบัน: {displayValue(k, cur)}</span>
        {changed && <Chip tone="info">แก้ไขแล้ว</Chip>}
      </div>
      {result}
    </div>
  );
}

function WifiList({ dev, linked }) {
  return (
    <div className="stack tight">
      <div className="row between">
        <span className="small bold">Wi-Fi ที่อุปกรณ์จำไว้ (5 ช่อง)</span>
      </div>
      <div className="list">
        {dev.wifi.map((w) => (
          <button key={w.slot} className="li" onClick={() => nav.push({ name: 'wifi-edit', slot: w.slot })} disabled={!linked} style={{ opacity: linked ? 1 : 0.6 }}>
            <span className="chip outline num" style={{ minWidth: 34, justifyContent: 'center' }}>{w.slot}</span>
            <div className="grow">
              {w.ssid ? (
                <>
                  <span className="t">{w.ssid}</span>
                  <span className="s">{w.dhcp ? 'รับ IP อัตโนมัติ (DHCP)' : `IP คงที่ ${w.ip}`}</span>
                </>
              ) : (
                <span className="s">ช่องว่าง · แตะเพื่อเพิ่ม</span>
              )}
            </div>
            <Icon name={w.ssid ? 'pencil' : 'plus'} size={18} className="muted" />
          </button>
        ))}
      </div>
      {!linked && <div className="tiny muted">เชื่อมต่อก่อนเพื่อแก้ไข Wi-Fi</div>}
    </div>
  );
}

const IPV4 = /^(25[0-5]|2[0-4]\d|1?\d?\d)(\.(25[0-5]|2[0-4]\d|1?\d?\d)){3}$/;
const ipNum = (ip) => ip.split('.').reduce((a, b) => (a << 8) + Number(b), 0) >>> 0;

export function WifiEdit({ slot }) {
  const s = useApp();
  const dev = findDevice(s, s.selectedSn);
  const linked = isLinked(s, dev.sn);
  const cur = dev.wifi[slot - 1];
  const isNew = !cur.ssid;
  const [f, setF] = useState({
    ssid: cur.ssid || '',
    pass: '',
    open: false,
    dhcp: cur.ssid ? cur.dhcp : true,
    ip: cur.ip || '',
    gateway: cur.gateway || '',
    subnet: cur.subnet || '255.255.255.0',
    dns: cur.dns || '',
  });
  const [touched, setTouched] = useState(false);
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState(null);
  const [confirmDel, setConfirmDel] = useState(false);

  const e = {};
  if (!f.ssid.trim() || f.ssid.length > 32) e.ssid = 'ชื่อเครือข่าย 1–32 ตัวอักษร';
  const renamed = !isNew && f.ssid !== cur.ssid;
  const needPass = isNew || renamed;
  if (!f.open && (needPass || f.pass) && (f.pass.length < 8 || f.pass.length > 63)) e.pass = needPass && !f.pass ? 'ต้องใส่รหัสผ่าน (8–63 ตัว) สำหรับเครือข่ายใหม่หรือเปลี่ยนชื่อ' : 'รหัสผ่าน 8–63 ตัวอักษร';
  if (!f.dhcp) {
    if (!IPV4.test(f.ip)) e.ip = 'รูปแบบ IP ไม่ถูกต้อง เช่น 192.168.1.50';
    if (!IPV4.test(f.subnet)) e.subnet = 'รูปแบบ Subnet ไม่ถูกต้อง เช่น 255.255.255.0';
    if (!IPV4.test(f.gateway)) e.gateway = 'รูปแบบ Gateway ไม่ถูกต้อง';
    else if (!e.ip && !e.subnet && (ipNum(f.ip) & ipNum(f.subnet)) !== (ipNum(f.gateway) & ipNum(f.subnet))) e.gateway = 'Gateway ต้องอยู่วงเดียวกับ IP';
    if (f.dns && !IPV4.test(f.dns)) e.dns = 'รูปแบบ DNS ไม่ถูกต้อง หรือเว้นว่าง';
  }
  const ok = Object.keys(e).length === 0;
  const show = (k) => (touched ? e[k] : null);

  const run = async (op) => {
    setTouched(true);
    if (op !== 'del' && !ok) return;
    setBusy(true);
    setErr(null);
    try {
      const res = await wifiCommand(dev.sn, op, { slot, ssid: f.ssid.trim(), dhcp: f.dhcp, ip: f.ip, gateway: f.gateway, subnet: f.subnet, dns: f.dns });
      if (!res.ok) setErr(res.msg);
      else {
        toast(op === 'del' ? `ลบ Wi-Fi ช่อง ${slot} แล้ว` : `บันทึก Wi-Fi ช่อง ${slot} ในอุปกรณ์แล้ว`, 'ok');
        nav.back();
      }
    } catch (x) {
      if (x instanceof BleLost) setErr('การเชื่อมต่อหลุด ยังไม่ได้บันทึก เชื่อมต่อใหม่แล้วลองอีกครั้ง');
      else throw x;
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="screen">
      <AppBar title={isNew ? `เพิ่ม Wi-Fi ช่อง ${slot}` : `แก้ไข Wi-Fi ช่อง ${slot}`} sub={dev.name} />
      <div className="content stack with-footer">
        {!linked && <NeedConnection dev={dev} text="ต้องเชื่อมต่อเพื่อบันทึก Wi-Fi ลงอุปกรณ์" />}
        <Field label="ชื่อเครือข่าย (SSID)" htmlFor="ssid" error={show('ssid')}>
          <input id="ssid" className={`input ${show('ssid') ? 'invalid' : ''}`} maxLength={32} value={f.ssid} onChange={(x) => setF({ ...f, ssid: x.target.value })} />
        </Field>
        <Field label="รหัสผ่าน" htmlFor="wpass" error={show('pass')} help={isNew ? '8–63 ตัวอักษร' : 'ระบบไม่แสดงรหัสผ่านเดิม เว้นว่างไว้เพื่อใช้รหัสเดิม'}>
          <input id="wpass" type="password" autoComplete="new-password" className={`input ${show('pass') ? 'invalid' : ''}`} disabled={f.open} placeholder={isNew ? '' : '•••••••• (ใช้รหัสเดิม)'} value={f.pass} onChange={(x) => setF({ ...f, pass: x.target.value })} />
        </Field>
        {(isNew || renamed) && (
          <label className="check small">
            <input type="checkbox" checked={f.open} onChange={(x) => setF({ ...f, open: x.target.checked, pass: '' })} />
            เครือข่ายเปิด (ไม่มีรหัสผ่าน)
          </label>
        )}
        <Field label="การรับ IP">
          <Seg label="การรับ IP" value={f.dhcp ? 'dhcp' : 'static'} onChange={(val) => setF({ ...f, dhcp: val === 'dhcp' })} options={[{ value: 'dhcp', label: 'อัตโนมัติ (DHCP)' }, { value: 'static', label: 'IP คงที่' }]} />
        </Field>
        {!f.dhcp && (
          <div className="card stack">
            <Field label="IP" htmlFor="ip" error={show('ip')}><input id="ip" className="input num" inputMode="decimal" value={f.ip} onChange={(x) => setF({ ...f, ip: x.target.value.trim() })} placeholder="192.168.1.50" /></Field>
            <Field label="Subnet" htmlFor="subnet" error={show('subnet')}><input id="subnet" className="input num" inputMode="decimal" value={f.subnet} onChange={(x) => setF({ ...f, subnet: x.target.value.trim() })} /></Field>
            <Field label="Gateway" htmlFor="gw" error={show('gateway')}><input id="gw" className="input num" inputMode="decimal" value={f.gateway} onChange={(x) => setF({ ...f, gateway: x.target.value.trim() })} placeholder="192.168.1.1" /></Field>
            <Field label="DNS (ไม่บังคับ)" htmlFor="dns" error={show('dns')} help="เว้นว่าง = ใช้ Gateway"><input id="dns" className="input num" inputMode="decimal" value={f.dns} onChange={(x) => setF({ ...f, dns: x.target.value.trim() })} /></Field>
          </div>
        )}
        {!isNew && <div className="tiny muted">ถ้าอุปกรณ์กำลังใช้เครือข่ายนี้ จะตัดแล้วต่อใหม่ด้วยค่าที่แก้ทันที</div>}
        {err && <Banner tone="bad">{err}</Banner>}
        {!isNew && !confirmDel && (
          <Btn kind="ghost" icon="trash" style={{ color: 'var(--bad)', alignSelf: 'flex-start' }} disabled={!linked || busy} onClick={() => setConfirmDel(true)}>
            ลบเครือข่ายนี้
          </Btn>
        )}
        {confirmDel && (
          <Banner tone="bad" title={`ลบ ${cur.ssid} ออกจากช่อง ${slot}?`} actions={<><Btn size="sm" kind="danger" busy={busy} onClick={() => run('del')}>ลบ</Btn><Btn size="sm" kind="secondary" onClick={() => setConfirmDel(false)}>ไม่ลบ</Btn></>}>
            ช่องอื่นไม่เปลี่ยน
          </Banner>
        )}
      </div>
      <div className="footer-bar">
        <Btn kind="teal" block busy={busy} disabled={!linked} onClick={() => run(isNew ? 'add' : 'edit')}>
          {busy ? 'กำลังบันทึก…' : 'บันทึกลงอุปกรณ์'}
        </Btn>
      </div>
    </div>
  );
}

/** The network the box is on right now (WIFI event, ble-protocol.md). */
function WifiNow({ dev }) {
  const w = dev.wifiNow || {};
  return (
    <div className="stack tight">
      <span className="small bold">Wi-Fi ที่อุปกรณ์ต่ออยู่ตอนนี้</span>
      {w.connected ? (
        <dl className="kv">
          <dt>เครือข่าย</dt><dd>{w.ssid}</dd>
          <dt>สัญญาณ</dt><dd className="num">{w.rssi} dBm</dd>
          <dt>การรับ IP</dt><dd>{w.dhcp ? 'อัตโนมัติ (DHCP)' : 'IP คงที่'}</dd>
          <dt>IP</dt><dd className="mono">{w.ip}</dd>
          <dt>Gateway</dt><dd className="mono">{w.gateway}</dd>
          <dt>Subnet</dt><dd className="mono">{w.subnet}</dd>
          <dt>DNS</dt><dd className="mono">{w.dns || '—'}</dd>
        </dl>
      ) : (
        <div className="small muted">ไม่ได้เชื่อมต่อ Wi-Fi</div>
      )}
    </div>
  );
}
