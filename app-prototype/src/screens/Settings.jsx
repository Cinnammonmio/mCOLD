// Device settings: show current values → edit → save/cancel → per-value result.
// Values and ranges follow docs/app/device-settings.md. Wi-Fi uses its own per-slot commands.
import { useMemo, useState } from 'react';
import { nav, toast, useApp, TRIP_LOCKED } from '../store.js';
import { applyConfig, BleLost, CONFIG_RULES, deviceView, findDevice, isLinked, wifiCommand } from '../sim.js';
import { fmtPeriod, fmtTime, fmtTz } from '../format.js';
import { requestConnect } from '../ConnectSheet.jsx';
import { AppBar, Banner, Btn, Chip, Field, Icon, Seg, Switch } from '../ui.jsx';
import { DeviceContext, NeedConnection } from './Mcold.jsx';

// UI fields: how each device value is shown and edited (unit conversion lives here).
const FIELDS = {
  sample_period_s: { label: 'รอบบันทึกอุณหภูมิ', unit: 'นาที', toUi: (v) => v / 60, toDev: (u) => Math.round(u * 60), min: 1, max: 60, step: 1, def: 5, help: 'ค่าเริ่มต้น 5 นาที · ตั้งได้ 1–60 นาที · ยิ่งถี่ยิ่งใช้แบต' },
  temp_adj_c100: { label: 'ปรับค่าอุณหภูมิ', unit: '°C', toUi: (v) => v / 100, toDev: (u) => Math.round(u * 100), min: -10, max: 10, step: 0.01, def: 0, help: 'บวกหรือลบจากค่าที่วัดได้ −10.00 ถึง +10.00°C ใช้ทั้งบนจอกล่องและในข้อมูลที่ส่ง' },
  alarm_dwell_s: { label: 'เวลานอกช่วงก่อนแจ้งเตือน', unit: 'นาที', toUi: (v) => v / 60, toDev: (u) => Math.round(u * 60), min: 0, max: 60, step: 1, def: 5, help: 'อุณหภูมิต้องอยู่นอกช่วงต่อเนื่องนานเท่านี้ก่อนกล่องเตือน 0 = เตือนทันที · 0–60 นาที' },
  alarm_hyst_c10: { label: 'ระยะกลับเข้าช่วง (Hysteresis)', unit: '°C', toUi: (v) => v / 10, toDev: (u) => Math.round(u * 10), min: 0, max: 10, step: 0.1, def: 0.5, help: 'อุณหภูมิต้องกลับเข้ามาในช่วงเกินเส้นเท่านี้ก่อนถือว่าการเตือนจบ กันการเตือนสลับไปมาเมื่อค่าแกว่งตรงเส้น · 0–10.0°C' },
  gnss_period_s: { label: 'รอบหาตำแหน่ง GPS ระหว่างทริป', kind: 'select', options: [300, 600, 900, 1800, 3600, 7200, 21600, 43200, 86400].map((s) => ({ value: s, label: fmtPeriod(s) })), help: 'ค่าเริ่มต้น 30 นาที · ยิ่งถี่ยิ่งใช้แบต' },
  tz_offset_min: { label: 'เขตเวลา', kind: 'select', options: [-300, 0, 60, 330, 420, 480, 540, 600].map((m) => ({ value: m, label: fmtTz(m) + (m === 420 ? ' (ประเทศไทย)' : '') })), help: 'ใช้กับวันที่ของทริปและเวลาในไฟล์ข้อมูล ค่าเริ่มต้น UTC+7' },
  buzzer_enabled: { label: 'เสียงเตือนที่กล่อง', kind: 'switch' },
  led_front_pct: { label: 'ความสว่างไฟหน้ากล่อง', unit: '%', toUi: (v) => v, toDev: (u) => Math.round(u), min: 1, max: 100, step: 1, def: 2, help: '1–100% ค่าเริ่มต้น 2%' },
  led_bright_pct: { label: 'ความสว่างไฟชาร์จด้านข้าง', unit: '%', toUi: (v) => v, toDev: (u) => Math.round(u), min: 1, max: 100, step: 1, def: 20, help: '1–100% ค่าเริ่มต้น 20%' },
  upload_period_s: { label: 'รอบส่งข้อมูลผ่าน Wi-Fi ของอุปกรณ์', kind: 'select', options: [60, 300, 600, 900, 1800, 3600, 21600, 86400].map((s) => ({ value: s, label: fmtPeriod(s) })), help: 'ความถี่ที่กล่องเปิด Wi-Fi ของตัวเองเพื่อส่งสถานะ ไม่เกี่ยวกับการส่งข้อมูลผ่านมือถือ' },
};

const GROUPS = [
  { id: 'log', title: 'การบันทึกและอุณหภูมิ', icon: 'temperature', keys: ['sample_period_s', 'temp_adj_c100'] },
  { id: 'alarm', title: 'การแจ้งเตือน', icon: 'bell-ringing', keys: ['alarm_dwell_s', 'alarm_hyst_c10'] },
  { id: 'loc', title: 'ตำแหน่งและเวลา', icon: 'map-pin', keys: ['gnss_period_s', 'tz_offset_min'] },
  { id: 'led', title: 'ไฟและเสียง', icon: 'sun', keys: ['buzzer_enabled', 'led_front_pct', 'led_bright_pct'] },
];

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
  if (f.kind === 'select') return f.options.find((o) => o.value === devVal)?.label || String(devVal);
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

  const keys = useMemo(() => [...GROUPS.flatMap((g) => g.keys), 'upload_period_s'], []);
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
            {!showAdv && <div className="small muted">Wi-Fi ของอุปกรณ์ และรอบส่งข้อมูลผ่าน Wi-Fi สำหรับผู้ดูแลระบบ</div>}
            {showAdv && (
              <>
                <SettingRow k="upload_period_s" dev={dev} editing={editing} locked={false} draft={draft} setDraft={setDraft} error={errors.upload_period_s} result={rowResult('upload_period_s')} />
                <Banner tone="info">
                  การส่งข้อมูลทริปผ่านมือถือทำที่แท็บ <b>ส่งข้อมูล</b> และไม่ใช้รอบนี้
                </Banner>
                <WifiList dev={dev} linked={linked} />
                <div className="tiny muted">การตั้งค่า Server ปลายทางของอุปกรณ์ทำได้ที่โรงงานเท่านั้น</div>
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
            </dl>
            <div className="tiny muted">อ่านอย่างเดียว</div>
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
        {locked && <div className="lockline"><Icon name="lock" size={14} /> แก้ไขได้หลังจบทริป</div>}
        {!locked && k === 'alarm_hyst_c10' && <div className="tiny muted">กันการเตือนสลับไปมาเมื่ออุณหภูมิแกว่งอยู่ตรงเส้น</div>}
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
