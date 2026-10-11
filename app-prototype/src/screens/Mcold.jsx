// mCOLD: device list → tap NFC to connect → the connected box's menu (trip / settings / send).
// A box is not tied to one shipping box, so devices are named by their own number and the
// cargo label belongs to the trip. The three functions show only once a box is connected.
import { useState } from 'react';
import { nav, useApp, simNow } from '../store.js';
import { deviceView, disconnect, findDevice, inRange, isLinked, pendingTrips } from '../sim.js';
import { fmtAgo, fmtPct, fmtTemp, fmtTime } from '../format.js';
import { requestConnect } from '../ConnectSheet.jsx';
import { AppBar, Banner, BatteryIcon, Btn, Chip, Icon, Seg, useTick } from '../ui.jsx';
import { TripTab } from './Trip.jsx';
import { SettingsTab } from './Settings.jsx';
import { SendTab } from './Send.jsx';

/** Connect (NFC tap) and open that box's menu. `sn` null = whichever box the phone touches. */
export async function connectAndOpen(sn) {
  const got = await requestConnect(sn);
  if (got) nav.toTab('device');
}

function NetIndicator() {
  const s = useApp();
  return (
    <span className={`chip ${s.internet ? 'outline' : 'warn'}`} title="อินเทอร์เน็ตของมือถือ" style={{ marginRight: 4 }}>
      <Icon name={s.internet ? 'world' : 'world-off'} size={14} stroke={2} />
      {s.internet ? 'ออนไลน์' : 'ออฟไลน์'}
    </span>
  );
}

/** Separate states, never merged into one: BLE link, trip, delivery to server (internet is phone-wide). */
export function StateChips({ dev, v }) {
  const s = useApp();
  const linked = isLinked(s, dev.sn);
  const pending = pendingTrips(s, dev).length;
  return (
    <div className="status-line">
      <Chip tone={linked ? 'ok' : ''} icon={linked ? 'bluetooth-connected' : 'bluetooth-off'}>
        {linked ? 'เชื่อมต่ออยู่' : 'ไม่ได้เชื่อมต่อ'}
      </Chip>
      {s.pendingCmd[dev.sn] ? (
        <Chip tone="warn" icon="alert-triangle">รอตรวจสอบสถานะทริป</Chip>
      ) : v.known ? (
        dev.activeTripId ? <Chip tone="info" dot>กำลังบันทึกทริป</Chip> : <Chip tone="outline">ว่าง</Chip>
      ) : (
        <Chip tone="outline">สถานะทริป —</Chip>
      )}
      {v.known && pending > 0 && <Chip tone="warn" icon="cloud-upload">รอส่ง {pending}</Chip>}
    </div>
  );
}

export function Freshness({ v }) {
  const now = simNow();
  if (!v.known) return <div className="freshness">ยังไม่เคยรับข้อมูลจากเครื่องนี้</div>;
  if (v.live)
    return (
      <div className="freshness live">
        <span className="chip ok" style={{ padding: '0 7px', lineHeight: '18px' }}>
          <span className="ldot" />
          สด
        </span>
        ข้อมูลจากอุปกรณ์ผ่าน Bluetooth
      </div>
    );
  return (
    <div className="freshness">
      <Icon name="history" size={14} />
      ข้อมูลล่าสุดที่ได้รับ {fmtTime(v.at)} ({fmtAgo(v.at, now)})
    </div>
  );
}

/** Cargo label of the running trip, e.g. "กล่องวัคซีน #12" (app-only data). */
export function cargoOf(s, dev) {
  return dev.activeTripId ? s.appTrips[dev.activeTripId]?.name || '' : '';
}

function DeviceRow({ dev }) {
  const s = useApp();
  const v = deviceView(s, dev);
  const ok = v.trip ? inRange(v.temp, v.trip) : null;
  const cargo = cargoOf(s, dev);
  const linked = isLinked(s, dev.sn);
  return (
    <button className={`card tap stack tight ${linked ? 'selected' : ''}`} onClick={() => (linked ? nav.toTab('device') : connectAndOpen(dev.sn))}>
      <div className="row" style={{ alignItems: 'flex-start' }}>
        <div className={`dev-icon ${v.known ? '' : 'off'}`}>
          <Icon name={linked ? 'bluetooth-connected' : 'snowflake'} size={22} />
        </div>
        <div className="grow">
          <div className="bold" style={{ fontSize: 16 }}>{dev.name}</div>
          <div className="tiny muted mono">{dev.sn}</div>
        </div>
        <div className="stack tight" style={{ alignItems: 'flex-end', gap: 0 }}>
          <span className={`tempbig ${v.live ? '' : 'stale'}`} style={{ fontSize: 20, color: ok === false ? 'var(--bad)' : undefined }}>
            {v.known ? (v.temp == null ? 'ไม่พบหัววัด' : fmtTemp(v.temp)) : '—'}
          </span>
          <span className="row tiny" style={{ gap: 3, color: v.battery != null && v.battery < 15 ? 'var(--bad)' : 'var(--muted)' }}>
            <BatteryIcon pct={v.battery} /> {fmtPct(v.battery)}
          </span>
        </div>
      </div>
      {cargo && v.trip && (
        <div className="small row" style={{ gap: 6 }}>
          <Icon name="package" size={15} className="muted" /> ติดกับ <b>{cargo}</b>
        </div>
      )}
      <StateChips dev={dev} v={v} />
      <Freshness v={v} />
    </button>
  );
}

export function McoldHome() {
  const s = useApp();
  useTick(15000);
  const [filter, setFilter] = useState('all');
  const linkedDev = s.ble ? findDevice(s, s.ble.sn) : null;
  const queued = Object.values(s.transfers).filter((t) => t.stage !== 'done').length;
  const list = s.devices.filter((d) =>
    filter === 'all' ? true : filter === 'trip' ? !!d.activeTripId : pendingTrips(s, d).length > 0,
  );
  const nTrip = s.devices.filter((d) => d.activeTripId).length;
  const nPending = s.devices.filter((d) => pendingTrips(s, d).length > 0).length;
  return (
    <div className="screen">
      <AppBar title="mCOLD" backLabel="กลับ" right={<NetIndicator />} />
      <div className="content stack">
        {linkedDev ? (
          <button className="action" style={{ borderColor: 'var(--teal)' }} onClick={() => nav.toTab('device')}>
            <span className="ai teal"><Icon name="bluetooth-connected" /></span>
            <span className="grow">
              <span className="t">{linkedDev.name}</span>
              <div className="s">เชื่อมต่ออยู่ · แตะเพื่อเปิดเมนู</div>
            </span>
            <Icon name="chevron-right" className="muted" />
          </button>
        ) : null}
        <Btn kind="teal" block icon="nfc" onClick={() => connectAndOpen(null)} style={{ minHeight: 56, fontSize: 16 }}>
          แตะ NFC เพื่อเชื่อมต่อ
        </Btn>
        <Btn kind="secondary" block icon="player-play" onClick={() => nav.push({ name: 'multi-start' })}>
          เริ่มทริปหลายเครื่อง
        </Btn>

        {queued > 0 && (
          <Banner tone="warn" icon="database" title={`ข้อมูลค้างในแอป ${queued} ทริป`}>
            รับจากอุปกรณ์แล้วแต่ยังส่งไม่ครบ
          </Banner>
        )}

        <div className="section-title" style={{ marginTop: 6 }}>
          อุปกรณ์ <span className="muted">({s.devices.length})</span>
        </div>
        <Seg
          label="ตัวกรองอุปกรณ์"
          value={filter}
          onChange={setFilter}
          options={[
            { value: 'all', label: 'ทั้งหมด' },
            { value: 'trip', label: `กำลังบันทึก ${nTrip}` },
            { value: 'pending', label: `รอส่ง ${nPending}` },
          ]}
        />
        {list.map((d) => <DeviceRow key={d.sn} dev={d} />)}
        {list.length === 0 && <div className="card center small muted" style={{ padding: 24 }}>ไม่มีอุปกรณ์ในกลุ่มนี้</div>}
      </div>
    </div>
  );
}

/** Menu of the connected box. The three functions appear only while connected. */
export function DeviceHub() {
  const s = useApp();
  useTick(15000);
  const dev = findDevice(s, s.selectedSn);
  const v = deviceView(s, dev);
  const linked = isLinked(s, dev.sn);
  const pending = pendingTrips(s, dev).length;
  const cargo = cargoOf(s, dev);
  const ok = v.trip ? inRange(v.temp, v.trip) : null;
  const open = (name) => nav.push({ name });
  return (
    <div className="screen">
      <AppBar title={dev.name} sub={dev.sn} backLabel="อุปกรณ์" onBack={() => nav.toTab('devices')} />
      <div className="content stack">
        <div className="card raised stack">
          <StateChips dev={dev} v={v} />
          <div className="grid2">
            <div className="metric">
              <div className="label"><Icon name="temperature" size={14} /> อุณหภูมิ</div>
              <div className="value" style={{ color: ok === false ? 'var(--bad)' : undefined }}>
                {v.known && v.temp == null ? 'ไม่พบหัววัด' : fmtTemp(v.temp)}
              </div>
              <div className="hint">วัดเมื่อ {fmtTime(v.at)}</div>
            </div>
            <div className="metric">
              <div className="label"><BatteryIcon pct={v.battery} /> แบตเตอรี่</div>
              <div className="value" style={{ color: v.battery != null && v.battery < 15 ? 'var(--bad)' : undefined }}>{fmtPct(v.battery)}</div>
            </div>
          </div>
          {cargo && v.trip && (
            <div className="small row" style={{ gap: 6 }}>
              <Icon name="package" size={15} className="muted" /> ติดกับ <b>{cargo}</b>
            </div>
          )}
          {v.trip?.alarms.active.length > 0 && <div className="small" style={{ color: 'var(--bad)', fontWeight: 600 }}>มีการแจ้งเตือนค้างอยู่</div>}
          <Freshness v={v} />
        </div>

        {linked ? (
          <>
            <div className="action-grid">
              <button className="action" onClick={() => open('trip')}>
                <span className="ai"><Icon name={dev.activeTripId ? 'player-stop' : 'player-play'} /></span>
                <span className="grow t">เริ่ม / จบทริป</span>
                {dev.activeTripId && <Chip tone="info" dot>กำลังบันทึก</Chip>}
                <Icon name="chevron-right" className="muted" />
              </button>
              <button className="action" onClick={() => open('settings')}>
                <span className="ai slate"><Icon name="adjustments-horizontal" /></span>
                <span className="grow t">ตั้งค่าอุปกรณ์</span>
                <Icon name="chevron-right" className="muted" />
              </button>
              <button className="action" onClick={() => open('send')}>
                <span className="ai teal"><Icon name="cloud-upload" /></span>
                <span className="grow t">ส่งข้อมูล</span>
                {pending > 0 && <Chip tone="warn">{pending}</Chip>}
                <Icon name="chevron-right" className="muted" />
              </button>
            </div>
            <Btn kind="ghost" icon="bluetooth-off" onClick={disconnect} style={{ alignSelf: 'center' }}>
              ตัดการเชื่อมต่อ
            </Btn>
          </>
        ) : (
          <Banner
            tone="neutral"
            icon="bluetooth-off"
            title="ไม่ได้เชื่อมต่อ"
            actions={<Btn kind="teal" size="sm" icon="nfc" onClick={() => requestConnect(dev.sn)}>แตะ NFC เพื่อเชื่อมต่อ</Btn>}
          >
            เชื่อมต่อเพื่อใช้งานเมนูของเครื่องนี้
          </Banner>
        )}
      </div>
    </div>
  );
}

const FN_TITLE = { trip: 'เริ่ม / จบทริป', settings: 'ตั้งค่าอุปกรณ์', send: 'ส่งข้อมูล' };

/** One of the three functions of the connected box. */
export function FunctionScreen({ fn }) {
  const s = useApp();
  useTick(15000);
  const dev = findDevice(s, s.selectedSn);
  return (
    <div className="screen">
      <AppBar title={FN_TITLE[fn]} sub={dev.name} backLabel="เมนู" onBack={() => nav.toTab('device')} />
      <div className="content stack" key={fn}>
        {fn === 'trip' && <TripTab />}
        {fn === 'settings' && <SettingsTab />}
        {fn === 'send' && <SendTab />}
      </div>
    </div>
  );
}

/** Which box this screen acts on, and whether it is connected. */
export function DeviceContext() {
  const s = useApp();
  const dev = findDevice(s, s.selectedSn);
  const linked = isLinked(s, dev.sn);
  return (
    <div className="card row" style={{ padding: '10px 12px' }}>
      <div className={`dev-icon ${linked ? '' : 'off'}`} style={{ width: 36, height: 36 }}>
        <Icon name={linked ? 'bluetooth-connected' : 'bluetooth-off'} size={19} />
      </div>
      <div className="grow">
        <div className="bold">{dev.name}</div>
        <div className="tiny muted">
          <span className="mono">{dev.sn}</span> · {linked ? <span style={{ color: 'var(--ok)', fontWeight: 600 }}>เชื่อมต่ออยู่</span> : 'ไม่ได้เชื่อมต่อ'}
        </div>
      </div>
      {!linked && (
        <Btn kind="teal" size="sm" icon="nfc" onClick={() => requestConnect(dev.sn)}>
          เชื่อมต่อ
        </Btn>
      )}
    </div>
  );
}

export function NeedConnection({ dev, text, reason }) {
  return (
    <div className="banner neutral">
      <Icon name="bluetooth-off" />
      <div className="grow">
        <b>ยังไม่ได้เชื่อมต่อกับ {dev.name}</b>
        <div className="small">{text}</div>
        <div className="actions">
          <Btn kind="teal" size="sm" icon="nfc" onClick={() => requestConnect(dev.sn, reason)}>
            แตะ NFC เพื่อเชื่อมต่อ
          </Btn>
        </div>
      </div>
    </div>
  );
}
