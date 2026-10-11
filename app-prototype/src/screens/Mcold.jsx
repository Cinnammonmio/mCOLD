// mCOLD shell: device context + bottom tabs for the three main functions.
import { useState } from 'react';
import { nav, update, useApp, simNow } from '../store.js';
import { deviceView, findDevice, inRange, isLinked, pendingTrips, disconnect } from '../sim.js';
import { fmtAgo, fmtPct, fmtTemp, fmtTime, tripLabel } from '../format.js';
import { requestConnect } from '../ConnectSheet.jsx';
import { AppBar, BatteryIcon, Btn, Chip, Icon, Sheet, useTick } from '../ui.jsx';
import { TripTab } from './Trip.jsx';
import { SettingsTab } from './Settings.jsx';
import { SendTab } from './Send.jsx';

const TABS = [
  { id: 'devices', label: 'อุปกรณ์', icon: 'device-mobile' },
  { id: 'trip', label: 'ทริป', icon: 'truck-delivery' },
  { id: 'settings', label: 'ตั้งค่า', icon: 'adjustments-horizontal' },
  { id: 'send', label: 'ส่งข้อมูล', icon: 'cloud-upload' },
];

export function McoldShell() {
  const s = useApp();
  useTick(15000);
  const tab = s.mcoldTab;
  const dev = findDevice(s, s.selectedSn);
  const pending = pendingTrips(s, dev).length;
  const setTab = (t) => update((st) => void (st.mcoldTab = t));
  return (
    <div className="screen">
      <AppBar title="mCOLD" sub="กล่องขนส่งยาควบคุมอุณหภูมิ" backLabel="ผลิตภัณฑ์" right={<NetIndicator />} />
      <div className="content stack" key={tab}>
        {tab === 'devices' && <DevicesTab />}
        {tab === 'trip' && <TripTab />}
        {tab === 'settings' && <SettingsTab />}
        {tab === 'send' && <SendTab />}
      </div>
      <nav className="tabbar" aria-label="ฟังก์ชัน mCOLD">
        {TABS.map((t) => (
          <button key={t.id} className={tab === t.id ? 'on' : ''} aria-current={tab === t.id ? 'page' : undefined} onClick={() => setTab(t.id)}>
            <Icon name={t.icon} size={22} />
            {t.label}
            {t.id === 'send' && pending > 0 && <span className="dot">{pending}</span>}
            {t.id === 'trip' && dev.activeTripId && <span className="dot" style={{ background: 'var(--info)' }}>●</span>}
          </button>
        ))}
      </nav>
    </div>
  );
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

/** Four separate states, never merged into one: BLE link, trip, delivery to server (internet is phone-wide). */
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
        dev.activeTripId ? (
          <Chip tone="info" dot>กำลังบันทึกทริป</Chip>
        ) : (
          <Chip tone="outline">ไม่มีทริปทำงาน</Chip>
        )
      ) : (
        <Chip tone="outline">สถานะทริป —</Chip>
      )}
      {v.known && (pending > 0 ? <Chip tone="warn" icon="cloud-upload">รอส่ง {pending} ทริป</Chip> : dev.trips.length > 0 ? <Chip tone="ok" icon="cloud-check">ไม่มีทริปรอส่ง</Chip> : null)}
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

function DeviceCard({ dev, selected, onSelect }) {
  const s = useApp();
  const v = deviceView(s, dev);
  const ok = v.trip ? inRange(v.temp, v.trip) : null;
  return (
    <button className={`card tap stack tight ${selected ? 'selected' : ''}`} onClick={onSelect} aria-pressed={selected}>
      <div className="row" style={{ alignItems: 'flex-start' }}>
        <div className={`dev-icon ${v.known ? '' : 'off'}`}>
          <Icon name="package" size={22} />
        </div>
        <div className="grow">
          <div className="row between">
            <span className="bold">{dev.name}</span>
            {selected && <Chip tone="teal" icon="check">เลือกอยู่</Chip>}
          </div>
          <div className="tiny muted mono">{dev.sn}</div>
        </div>
      </div>
      <div className="row between" style={{ alignItems: 'flex-end' }}>
        <div>
          <div className="tiny muted">อุณหภูมิล่าสุด</div>
          <div className={`tempbig ${v.live ? '' : 'stale'}`} style={{ fontSize: 24, color: ok === false ? 'var(--bad)' : undefined }}>
            {v.known ? (v.temp == null ? 'ไม่พบหัววัด' : fmtTemp(v.temp)) : '—'}
          </div>
        </div>
        <div className="row small" style={{ gap: 4 }} title="แบตเตอรี่">
          <BatteryIcon pct={v.battery} />
          <span className="num" style={{ color: v.battery != null && v.battery < 15 ? 'var(--bad)' : undefined }}>{fmtPct(v.battery)}</span>
        </div>
      </div>
      <StateChips dev={dev} v={v} />
      <Freshness v={v} />
    </button>
  );
}

function DevicesTab() {
  const s = useApp();
  const dev = findDevice(s, s.selectedSn);
  const v = deviceView(s, dev);
  const linked = isLinked(s, dev.sn);
  const pending = pendingTrips(s, dev).length;
  const queued = Object.values(s.transfers).filter((t) => t.stage !== 'done').length;
  const go = (tab) => update((st) => void (st.mcoldTab = tab));
  return (
    <>
      <div className="section-title">
        <Icon name="package" size={18} /> เครื่องที่เลือก
      </div>
      <div className="card raised stack">
        <div className="row" style={{ alignItems: 'flex-start' }}>
          <div className={`dev-icon ${v.known ? '' : 'off'}`}>
            <Icon name="package" size={22} />
          </div>
          <div className="grow">
            <div className="bold" style={{ fontSize: 17 }}>{dev.name}</div>
            <div className="tiny muted mono">{dev.sn}</div>
          </div>
          {linked ? (
            <Btn kind="secondary" size="sm" icon="bluetooth-off" onClick={disconnect}>
              ตัดการเชื่อมต่อ
            </Btn>
          ) : (
            <Btn kind="teal" size="sm" icon="nfc" onClick={() => requestConnect(dev.sn)}>
              เชื่อมต่อ
            </Btn>
          )}
        </div>
        <StateChips dev={dev} v={v} />
        <div className="grid2">
          <div className="metric">
            <div className="label"><Icon name="temperature" size={14} /> อุณหภูมิ</div>
            <div className="value" style={{ color: v.trip && inRange(v.temp, v.trip) === false ? 'var(--bad)' : undefined }}>
              {v.known && v.temp == null ? 'ไม่พบหัววัด' : fmtTemp(v.temp)}
            </div>
            <div className="hint">วัดเมื่อ {fmtTime(v.at)}</div>
          </div>
          <div className="metric">
            <div className="label"><BatteryIcon pct={v.battery} /> แบตเตอรี่</div>
            <div className="value">{fmtPct(v.battery)}</div>
            <div className="hint">{v.battery != null && v.battery < 15 ? 'ต่ำ ควรชาร์จ' : ' '}</div>
          </div>
        </div>
        {v.trip && (
          <div className="banner neutral" style={{ padding: '9px 12px' }}>
            <Icon name="truck-delivery" />
            <div className="grow small">
              <b>{s.appTrips[v.trip.trip_id]?.name || tripLabel(v.trip)}</b> กำลังบันทึก · ช่วงที่อนุญาต {fmtTemp(v.trip.low)} ถึง {fmtTemp(v.trip.high)}
              {v.trip.alarms.active.length > 0 && <div style={{ color: 'var(--bad)', fontWeight: 600 }}>มีการแจ้งเตือนค้างอยู่</div>}
            </div>
          </div>
        )}
        <Freshness v={v} />
      </div>

      <div className="action-grid">
        <button className="action" onClick={() => go('trip')}>
          <span className="ai"><Icon name={dev.activeTripId ? 'player-stop' : 'player-play'} /></span>
          <span className="grow">
            <span className="t">เริ่มและจบทริป</span>
            <div className="s">{dev.activeTripId ? 'ดูทริปที่กำลังทำงาน หรือจบทริป' : 'ตั้งช่วงอุณหภูมิแล้วเริ่มบันทึก'}</div>
          </span>
          <Icon name="chevron-right" className="muted" />
        </button>
        <button className="action" onClick={() => go('settings')}>
          <span className="ai slate"><Icon name="adjustments-horizontal" /></span>
          <span className="grow">
            <span className="t">ตั้งค่าอุปกรณ์</span>
            <div className="s">รอบบันทึก การแจ้งเตือน ไฟและเสียง</div>
          </span>
          <Icon name="chevron-right" className="muted" />
        </button>
        <button className="action" onClick={() => go('send')}>
          <span className="ai teal"><Icon name="cloud-upload" /></span>
          <span className="grow">
            <span className="t">เลือกและส่งข้อมูล</span>
            <div className="s">{pending > 0 ? `มีทริปรอส่ง ${pending} ทริป` : 'ส่งข้อมูลทริปที่จบแล้วขึ้น Server'}</div>
          </span>
          {pending > 0 && <Chip tone="warn">{pending}</Chip>}
          <Icon name="chevron-right" className="muted" />
        </button>
      </div>

      {queued > 0 && (
        <div className="banner warn">
          <Icon name="database" />
          <div className="grow small">
            <b>ข้อมูลค้างในแอป {queued} ทริป</b>
            <div>ข้อมูลที่รับจากอุปกรณ์แล้วแต่ยังส่งไม่ครบ ดูได้ที่แท็บส่งข้อมูล</div>
          </div>
        </div>
      )}

      <div className="section-title" style={{ justifyContent: 'space-between' }}>
        <span className="row"><Icon name="list-check" size={18} /> อุปกรณ์ทั้งหมด <span className="muted">({s.devices.length})</span></span>
      </div>
      {s.devices.map((d) => (
        <DeviceCard key={d.sn} dev={d} selected={d.sn === s.selectedSn} onSelect={() => update((st) => void (st.selectedSn = d.sn))} />
      ))}
      <p className="tiny muted center" style={{ margin: 0 }}>
        เพิ่มเครื่องใหม่: แตะ NFC บนกล่องที่ยังไม่อยู่ในรายการ (ยังไม่จำลองในต้นแบบนี้)
      </p>
    </>
  );
}

/** Compact "working on this box" header with a switcher, used on trip / settings / send tabs. */
export function DeviceContext({ lockedReason }) {
  const s = useApp();
  const [open, setOpen] = useState(false);
  const dev = findDevice(s, s.selectedSn);
  const linked = isLinked(s, dev.sn);
  return (
    <>
      <div className="card row" style={{ padding: '10px 12px' }}>
        <div className={`dev-icon ${linked ? '' : 'off'}`} style={{ width: 36, height: 36 }}>
          <Icon name={linked ? 'bluetooth-connected' : 'package'} size={19} />
        </div>
        <div className="grow">
          <div className="bold">{dev.name}</div>
          <div className="tiny muted">
            <span className="mono">{dev.sn}</span> · {linked ? <span style={{ color: 'var(--ok)', fontWeight: 600 }}>เชื่อมต่ออยู่</span> : 'ไม่ได้เชื่อมต่อ'}
          </div>
        </div>
        <Btn kind="ghost" size="sm" onClick={() => setOpen(true)} disabled={!!lockedReason} title={lockedReason}>
          เปลี่ยน
        </Btn>
      </div>
      {open && (
        <Sheet onClose={() => setOpen(false)} label="เลือกอุปกรณ์">
          <h2 style={{ marginBottom: 10 }}>เลือกอุปกรณ์</h2>
          <div className="list">
            {s.devices.map((d) => {
              const v = deviceView(s, d);
              return (
                <button
                  key={d.sn}
                  className="li"
                  onClick={() => {
                    update((st) => void (st.selectedSn = d.sn));
                    setOpen(false);
                  }}
                >
                  <div className={`dev-icon ${v.known ? '' : 'off'}`} style={{ width: 36, height: 36 }}>
                    <Icon name="package" size={18} />
                  </div>
                  <div className="grow">
                    <span className="t">{d.name}</span>
                    <span className="s">
                      {d.activeTripId ? 'กำลังบันทึกทริป' : 'ไม่มีทริปทำงาน'} · รอส่ง {pendingTrips(s, d).length}
                    </span>
                  </div>
                  {d.sn === s.selectedSn && <Icon name="check" className="muted" />}
                </button>
              );
            })}
          </div>
        </Sheet>
      )}
    </>
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
