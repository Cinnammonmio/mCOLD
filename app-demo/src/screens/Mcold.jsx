// mCOLD: device list → tap NFC to connect → the connected box's menu (trip / settings / send).
// A box is not tied to one shipping box, so devices are named by their own number and the
// cargo label belongs to the trip. The three functions show only once a box is connected.
import { useState } from 'react';
import { nav, useApp, simNow, wardDevices, wardOf, userName } from '../store.js';
import { WardBar } from '../Users.jsx';
import { deviceView, disconnect, findDevice, inRange, isLinked, pendingTrips } from '../sim.js';
import { fmtAgo, fmtDuration, fmtPct, fmtTemp, fmtTime } from '../format.js';
import { DeviceReadout, requestConnect } from '../ConnectSheet.jsx';
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

/** One box per row, sized so five fill the phone screen; the rest scroll. */
function DeviceRow({ dev }) {
  const s = useApp();
  const v = deviceView(s, dev);
  const ok = v.trip ? inRange(v.temp, v.trip) : null;
  const cargo = cargoOf(s, dev);
  const linked = isLinked(s, dev.sn);
  const pending = pendingTrips(s, dev).length;
  const meta = v.trip ? s.appTrips[v.trip.trip_id] : null;
  return (
    <button className="li" onClick={() => (linked ? nav.toTab('device') : connectAndOpen(dev.sn))} style={{ alignItems: 'stretch', padding: '12px 12px', minHeight: 108 }}>
      <div className={`dev-icon ${v.known ? '' : 'off'}`} style={{ width: 40, height: 40 }}>
        <Icon name={linked ? 'bluetooth-connected' : 'snowflake'} size={21} />
      </div>
      <div className="grow" style={{ gap: 4 }}>
        <div className="row between" style={{ alignItems: 'flex-start' }}>
          <div style={{ minWidth: 0 }}>
            <div className="bold" style={{ fontSize: 16 }}>{dev.name}</div>
            <div className="tiny muted mono">{dev.sn}</div>
          </div>
          <span className="tempbig" style={{ fontSize: 22, color: ok === false ? 'var(--bad)' : v.live ? undefined : 'var(--muted)' }}>
            {v.known ? (v.temp == null ? 'ไม่พบหัววัด' : fmtTemp(v.temp)) : '—'}
          </span>
        </div>
        <div className="row between" style={{ gap: 6 }}>
          <span className="row wrap" style={{ gap: 4 }}>
            {linked && <Chip tone="ok" icon="bluetooth-connected">เชื่อมต่ออยู่</Chip>}
            {!v.known ? (
              <Chip tone="outline">ยังไม่เคยเชื่อมต่อ</Chip>
            ) : s.pendingCmd[dev.sn] ? (
              <Chip tone="warn">รอตรวจสอบทริป</Chip>
            ) : v.trip ? (
              <Chip tone="info" dot>กำลังบันทึก</Chip>
            ) : (
              <Chip tone="outline">ว่าง</Chip>
            )}
            {v.trip?.alarms.active.length > 0 && <Chip tone="bad" icon="bell-ringing">แจ้งเตือน</Chip>}
            {pending > 0 && <Chip tone="warn">รอส่ง {pending}</Chip>}
          </span>
          <span className="tiny num" style={{ whiteSpace: 'nowrap', color: v.battery != null && v.battery < 15 ? 'var(--bad)' : 'var(--muted)' }}>
            <BatteryIcon pct={v.battery} /> {fmtPct(v.battery)}
          </span>
        </div>
        <div className="tiny muted" style={{ whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
          {v.trip
            ? `${cargo ? `ติดกับ ${cargo} · ` : ''}${meta?.startedBy ? `เริ่มโดย ${userName(s, meta.startedBy.by).split(' ')[0]} · ` : ''}${fmtDuration(v.at - v.trip.startUtc)}`
            : ''}
          {v.trip ? ' · ' : ''}
          {v.live ? 'สด' : v.known ? `ข้อมูลเมื่อ ${fmtTime(v.at)}` : ''}
        </div>
      </div>
    </button>
  );
}

export function McoldHome() {
  const s = useApp();
  useTick(15000);
  const [filter, setFilter] = useState('all');
  const linkedDev = s.ble ? findDevice(s, s.ble.sn) : null;
  const queued = Object.values(s.transfers).filter((t) => t.stage !== 'done').length;
  const mine = wardDevices(s);
  const list = mine.filter((d) =>
    filter === 'all' ? true : filter === 'trip' ? !!d.activeTripId : pendingTrips(s, d).length > 0,
  );
  const nTrip = mine.filter((d) => d.activeTripId).length;
  const nPending = mine.filter((d) => pendingTrips(s, d).length > 0).length;
  return (
    <div className="screen">
      <AppBar title="mCOLD" backLabel="กลับ" right={<NetIndicator />} />
      <div className="content stack" style={{ gap: 10 }}>
        <WardBar />
        {linkedDev && (
          <button className="action" style={{ borderColor: 'var(--teal)', minHeight: 52, padding: '8px 12px' }} onClick={() => nav.toTab('device')}>
            <span className="ai teal" style={{ width: 34, height: 34 }}><Icon name="bluetooth-connected" size={18} /></span>
            <span className="grow">
              <span className="t">{linkedDev.name}</span>
              <span className="s"> · เชื่อมต่ออยู่ แตะเพื่อเปิดเมนู</span>
            </span>
            <Icon name="chevron-right" className="muted" />
          </button>
        )}
        <div className="row" style={{ gap: 8 }}>
          <Btn kind="teal" icon="nfc" className="grow" onClick={() => connectAndOpen(null)} style={{ minHeight: 52 }}>
            แตะ NFC เชื่อมต่อ
          </Btn>
          <Btn kind="secondary" icon="player-play" onClick={() => nav.push({ name: 'multi-start' })} style={{ minHeight: 52, padding: '0 12px' }}>
            เริ่มหลายเครื่อง
          </Btn>
        </div>
        {queued > 0 && (
          <div className="row small" style={{ color: 'var(--warn)', fontWeight: 600 }}>
            <Icon name="database" size={16} /> ข้อมูลค้างในแอป {queued} ทริป ยังส่งไม่ครบ
          </div>
        )}
        <Seg
          label="ตัวกรองอุปกรณ์"
          value={filter}
          onChange={setFilter}
          options={[
            { value: 'all', label: `ทั้งหมด ${mine.length}` },
            { value: 'trip', label: `กำลังบันทึก ${nTrip}` },
            { value: 'pending', label: `รอส่ง ${nPending}` },
          ]}
        />
        <div className="list">
          {list.map((d) => <DeviceRow key={d.sn} dev={d} />)}
          {list.length === 0 && <div className="li small muted" style={{ justifyContent: 'center' }}>ไม่มีอุปกรณ์ในกลุ่มนี้</div>}
        </div>
        <div className="tiny muted" style={{ marginTop: -4 }}>อุณหภูมิและแบตเป็นค่าล่าสุดที่มือถือเครื่องนี้ได้รับ (เวลากำกับ) · "สด" = เชื่อมต่ออยู่</div>
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
  const [more, setMore] = useState(false);
  return (
    <div className="screen">
      <AppBar
        title={dev.name}
        sub={`${dev.sn} · ${wardOf(s, dev.wardId).name}`}
        backLabel="อุปกรณ์"
        onBack={() => nav.toTab('devices')}
        right={
          linked && (
            <button className="iconbtn" onClick={disconnect} aria-label="ตัดการเชื่อมต่อ" title="ตัดการเชื่อมต่อ">
              <Icon name="bluetooth-off" />
            </button>
          )
        }
      />
      <div className="content stack">
        <div className="card raised stack">
          <StateChips dev={dev} v={v} />
          {/* big numbers that fill their tiles; the reading time sits under them */}
          <div className="grid2">
            <div className="metric" style={{ padding: '10px 12px 12px' }}>
              <div className="label"><Icon name="temperature" size={14} /> อุณหภูมิ</div>
              <div className="num" style={{ fontWeight: 700, lineHeight: 1.1, letterSpacing: '-0.02em', fontSize: v.known && v.temp == null ? 22 : 40, color: ok === false ? 'var(--bad)' : undefined }}>
                {v.known && v.temp == null ? 'ไม่พบหัววัด' : fmtTemp(v.temp)}
              </div>
            </div>
            <div className="metric" style={{ padding: '10px 12px 12px' }}>
              <div className="label"><BatteryIcon pct={v.battery} /> แบตเตอรี่</div>
              <div className="num" style={{ fontWeight: 700, lineHeight: 1.1, letterSpacing: '-0.02em', fontSize: 40, color: v.battery != null && v.battery < 15 ? 'var(--bad)' : undefined }}>
                {fmtPct(v.battery)}
              </div>
            </div>
          </div>
          <div className="tiny muted">วัดเมื่อ {fmtTime(v.at)}</div>
          {cargo && v.trip && (
            <div className="small row" style={{ gap: 6 }}>
              <Icon name="package" size={15} className="muted" /> ติดกับ <b>{cargo}</b>
            </div>
          )}
          {v.trip && s.appTrips[v.trip.trip_id]?.startedBy && (
            <div className="tiny muted">ทริปเริ่มโดย {userName(s, s.appTrips[v.trip.trip_id].startedBy.by)} · {fmtTime(s.appTrips[v.trip.trip_id].startedBy.t)}</div>
          )}
          {v.trip?.alarms.active.length > 0 && <div className="small" style={{ color: 'var(--bad)', fontWeight: 600 }}>มีการแจ้งเตือนค้างอยู่</div>}
          <Freshness v={v} />
          {v.known && (
            <>
              <button
                className="row between"
                onClick={() => setMore(!more)}
                aria-expanded={more}
                aria-controls={`readout-${dev.sn}`}
                style={{ border: 0, borderTop: '1px solid var(--line)', background: 'none', padding: '10px 0 0', width: '100%', color: 'var(--teal)', fontWeight: 600, fontSize: 14 }}
              >
                {more ? 'ซ่อนข้อมูลเพิ่มเติม' : 'ดูข้อมูลเพิ่มเติม'}
                <Icon name="chevron-down" size={18} style={{ transform: more ? 'rotate(180deg)' : 'none' }} />
              </button>
              {more && <DeviceReadout sn={dev.sn} />}
            </>
          )}
        </div>

        {linked ? (
          <>
            <div className="action-grid">
              <button className="action" style={{ minHeight: 54, padding: '8px 12px' }} onClick={() => open('trip')}>
                <span className="ai"><Icon name={dev.activeTripId ? 'player-stop' : 'player-play'} /></span>
                <span className="grow t">เริ่ม / จบทริป</span>
                {dev.activeTripId && <Chip tone="info" dot>กำลังบันทึก</Chip>}
                <Icon name="chevron-right" className="muted" />
              </button>
              <button className="action" style={{ minHeight: 54, padding: '8px 12px' }} onClick={() => open('settings')}>
                <span className="ai slate"><Icon name="adjustments-horizontal" /></span>
                <span className="grow t">ตั้งค่าอุปกรณ์</span>
                <Icon name="chevron-right" className="muted" />
              </button>
              <button className="action" style={{ minHeight: 54, padding: '8px 12px' }} onClick={() => open('trips')}>
                <span className="ai" style={{ background: '#2463a8' }}><Icon name="chart-line" /></span>
                <span className="grow t">ข้อมูลทริป (กราฟ / ตาราง)</span>
                <span className="tiny muted">{dev.trips.length} ทริป</span>
                <Icon name="chevron-right" className="muted" />
              </button>
              <button className="action" style={{ minHeight: 54, padding: '8px 12px' }} onClick={() => open('send')}>
                <span className="ai teal"><Icon name="cloud-upload" /></span>
                <span className="grow t">ส่งข้อมูล</span>
                {pending > 0 && <Chip tone="warn">{pending}</Chip>}
                <Icon name="chevron-right" className="muted" />
              </button>
            </div>
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
      <AppBar
        title={FN_TITLE[fn]}
        sub={dev.name}
        backLabel="เมนู"
        onBack={() => nav.toTab('device')}
        right={
          isLinked(s, dev.sn) ? (
            <Chip tone="ok" icon="bluetooth-connected">เชื่อมต่ออยู่</Chip>
          ) : (
            <Btn kind="teal" size="sm" icon="nfc" onClick={() => requestConnect(dev.sn)}>เชื่อมต่อ</Btn>
          )
        }
      />
      <div className="content stack" key={fn}>
        {fn === 'trip' && <TripTab />}
        {fn === 'settings' && <SettingsTab />}
        {fn === 'send' && <SendTab />}
      </div>
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
