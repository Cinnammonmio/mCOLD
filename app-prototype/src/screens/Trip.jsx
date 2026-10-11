import { useState } from 'react';
import { nav, useApp, simNow, update } from '../store.js';
import { ackAlarm, BleLost, deviceView, findDevice, inRange, isLinked, pendingTrips } from '../sim.js';
import { fmtAgo, fmtDateTime, fmtDuration, fmtNum, fmtPct, fmtPeriod, fmtTemp, fmtTime, tripLabel } from '../format.js';
import { requestConnect } from '../ConnectSheet.jsx';
import { Banner, BatteryIcon, Btn, Chip, Icon } from '../ui.jsx';
import { toast } from '../store.js';
import { DeviceContext, Freshness, NeedConnection } from './Mcold.jsx';

export const EVENT_TEXT = {
  TRIP_START: 'เริ่มทริป',
  TRIP_STOP: 'จบทริป',
  ALARM_HIGH: 'อุณหภูมิสูงกว่าช่วงที่อนุญาต',
  ALARM_LOW: 'อุณหภูมิต่ำกว่าช่วงที่อนุญาต',
  ALARM_PROBE: 'ไม่ได้รับค่าจากหัววัด',
  BATTERY_LOW: 'แบตเตอรี่ต่ำ',
  ALARM_ACK: 'ผู้ใช้รับทราบการแจ้งเตือน',
};
export const eventText = (e) => (e.type === 'ALARM_CLEAR' ? `กลับสู่ปกติ: ${EVENT_TEXT[e.detail] || e.detail}` : EVENT_TEXT[e.type] || e.type);
const isAlarm = (t) => ['ALARM_HIGH', 'ALARM_LOW', 'ALARM_PROBE', 'BATTERY_LOW'].includes(t);

export function TripTab() {
  const s = useApp();
  const dev = findDevice(s, s.selectedSn);
  const v = deviceView(s, dev);
  const linked = isLinked(s, dev.sn);
  return (
    <>
      <DeviceContext />
      {!v.known ? (
        <NeedConnection dev={dev} text="แอปยังไม่เคยรับข้อมูลจากเครื่องนี้ เชื่อมต่อเพื่อดูสถานะและเริ่มทริป" />
      ) : v.trip ? (
        <ActiveTrip dev={dev} v={v} linked={linked} />
      ) : (
        <IdleTrip dev={dev} v={v} linked={linked} />
      )}
    </>
  );
}

function IdleTrip({ dev, v, linked }) {
  const s = useApp();
  const last = v.lastTrip;
  const pending = pendingTrips(s, dev).length;
  return (
    <>
      <div className="card stack center" style={{ alignItems: 'center', padding: '22px 16px' }}>
        <div className="dev-icon" style={{ width: 56, height: 56, borderRadius: 16 }}>
          <Icon name="player-play" size={28} />
        </div>
        <div>
          <div className="bold" style={{ fontSize: 17 }}>ไม่มีทริปที่กำลังทำงาน</div>
          <div className="small muted">เริ่มทริปเมื่อบรรจุสินค้าลงกล่องแล้ว อุปกรณ์จะบันทึกอุณหภูมิตามรอบที่ตั้งไว้</div>
        </div>
        <Btn kind="teal" block icon="player-play" onClick={() => nav.push({ name: 'start-trip' })}>
          เริ่มทริปใหม่
        </Btn>
        {!linked && <div className="tiny muted">ต้องแตะ NFC เพื่อเชื่อมต่อก่อนเริ่มทริป</div>}
      </div>
      <Freshness v={v} />
      {last && (
        <>
          <div className="section-title"><Icon name="history" size={18} /> ทริปล่าสุด</div>
          <div className="card stack tight">
            <div className="row between">
              <span className="bold">{s.appTrips[last.trip_id]?.name || tripLabel(last)}</span>
              {last.sent ? <Chip tone="ok">ส่งแล้ว</Chip> : <Chip tone="warn">รอส่ง</Chip>}
            </div>
            <div className="small muted">
              {fmtDateTime(last.startUtc)} – {fmtDateTime(last.stopUtc)} · {fmtDuration(last.stopUtc - last.startUtc)}
            </div>
          </div>
        </>
      )}
      {pending > 0 && (
        <Banner tone="warn" title={`มีทริปที่จบแล้วรอส่ง ${pending} ทริป`} actions={<Btn size="sm" kind="secondary" onClick={() => nav.toTab('send')}>ไปที่ส่งข้อมูล</Btn>}>
          ข้อมูลยังเก็บอยู่ในอุปกรณ์ เริ่มทริปใหม่ได้โดยข้อมูลเดิมไม่หาย
        </Banner>
      )}
    </>
  );
}

function AlarmBanner({ dev, v, linked }) {
  const a = v.trip.alarms;
  const [busy, setBusy] = useState(false);
  if (!a.active.length) return null;
  const titles = a.active.map((t) => EVENT_TEXT[t]).join(' · ');
  const ack = async () => {
    if (busy) return;
    setBusy(true);
    try {
      await ackAlarm(dev.sn);
      toast('รับทราบแล้ว อุปกรณ์ยังบันทึกต่อและเก็บประวัติไว้ครบ', 'ok');
    } catch (e) {
      if (e instanceof BleLost) toast('การเชื่อมต่อหลุด ยังไม่ได้รับทราบ ลองอีกครั้งหลังเชื่อมต่อ', 'bad');
    } finally {
      setBusy(false);
    }
  };
  return (
    <div className="banner bad" role="alert" style={{ border: a.acked ? '1px solid #f0c8c8' : 0 }}>
      <Icon name="bell-ringing" />
      <div className="grow">
        <b>{titles}</b>
        <div className="small">
          ตั้งแต่ {fmtTime(a.last?.t)}
          {a.last?.temp != null ? ` · วัดได้ ${fmtTemp(a.last.temp)}` : ''}
        </div>
        {a.acked ? (
          <div className="small" style={{ marginTop: 6 }}>
            <Icon name="bell-check" size={14} /> รับทราบแล้ว แต่ปัญหายังไม่หาย ตรวจสอบกล่องและสินค้า
          </div>
        ) : (
          <div className="actions">
            {linked ? (
              <Btn size="sm" kind="danger" busy={busy} icon="bell-check" onClick={ack}>
                รับทราบการแจ้งเตือน
              </Btn>
            ) : (
              <Btn size="sm" kind="secondary" icon="nfc" onClick={() => requestConnect(dev.sn, 'เชื่อมต่อเพื่อรับทราบการแจ้งเตือน')}>
                เชื่อมต่อเพื่อรับทราบ
              </Btn>
            )}
          </div>
        )}
        <div className="tiny" style={{ marginTop: 6, opacity: 0.85 }}>การรับทราบไม่ลบประวัติ และไม่ทำให้การแจ้งเตือนหายจนกว่าอุณหภูมิจะกลับเข้าช่วง</div>
      </div>
    </div>
  );
}

function ActiveTrip({ dev, v, linked }) {
  const s = useApp();
  const now = simNow();
  const trip = v.trip;
  const meta = s.appTrips[trip.trip_id];
  const ok = inRange(v.temp, trip);
  const g = v.gnss;
  return (
    <>
      {!linked && (
        <Banner
          tone="neutral"
          icon="history"
          title={`แสดงข้อมูลล่าสุดเมื่อ ${fmtTime(v.at)} (${fmtAgo(v.at, now)})`}
          actions={
            <Btn size="sm" kind="teal" icon="nfc" onClick={() => requestConnect(dev.sn)}>
              เชื่อมต่อเพื่อดูค่าปัจจุบัน
            </Btn>
          }
        >
          อุปกรณ์ยังบันทึกต่อแม้ไม่ได้เชื่อมต่อกับแอป
        </Banner>
      )}
      <AlarmBanner dev={dev} v={v} linked={linked} />

      <div className="card raised stack">
        <div className="row between" style={{ alignItems: 'flex-start' }}>
          <div className="grow">
            <div className="tiny muted">ทริปที่กำลังทำงาน</div>
            <div className="bold" style={{ fontSize: 17 }}>{meta?.name || tripLabel(trip)}</div>
            {meta?.name && <div className="tiny muted">{tripLabel(trip)}</div>}
          </div>
          <Chip tone="info" dot>กำลังบันทึก</Chip>
        </div>
        <div className="row between small">
          <span className="muted">เริ่ม {fmtDateTime(trip.startUtc)}</span>
          <span className="bold num">ผ่านไป {fmtDuration(v.at - trip.startUtc)}</span>
        </div>
        <div className="divider" />
        <div className="row between" style={{ alignItems: 'flex-end' }}>
          <div>
            <div className="tiny muted">{linked ? 'อุณหภูมิตอนนี้' : 'อุณหภูมิล่าสุดที่ได้รับ'}</div>
            <div className={`tempbig ${linked ? '' : 'stale'}`} style={{ color: ok === false ? 'var(--bad)' : undefined, fontSize: 38 }}>
              {v.temp == null ? 'ไม่พบหัววัด' : fmtTemp(v.temp, 2)}
            </div>
            <div className="tiny muted">วัดเมื่อ {fmtTime(v.at)}</div>
          </div>
          {ok === true && <Chip tone="ok" icon="check">อยู่ในช่วง</Chip>}
          {ok === false && <Chip tone="bad" icon="alert-triangle">นอกช่วง</Chip>}
          {ok === null && <Chip tone="warn">ไม่มีค่า</Chip>}
        </div>
        <div className="grid2">
          <div className="metric">
            <div className="label">ช่วงที่อนุญาต</div>
            <div className="value">{fmtTemp(trip.low)} – {fmtTemp(trip.high)}</div>
          </div>
          <div className="metric">
            <div className="label">ต่ำสุด / สูงสุดที่บันทึก</div>
            <div className="value">
              <span style={{ color: trip.min != null && trip.min < trip.low ? 'var(--bad)' : undefined }}>{fmtTemp(trip.min)}</span>
              {' / '}
              <span style={{ color: trip.max != null && trip.max > trip.high ? 'var(--bad)' : undefined }}>{fmtTemp(trip.max)}</span>
            </div>
          </div>
          <div className="metric">
            <div className="label"><Icon name="database" size={14} /> ข้อมูลในอุปกรณ์</div>
            <div className="value">{fmtNum(trip.rows)} แถว</div>
            <div className="hint">บันทึกทุก {fmtPeriod(trip.period)}</div>
          </div>
          <div className="metric">
            <div className="label"><BatteryIcon pct={v.battery} /> แบตเตอรี่</div>
            <div className="value" style={{ color: v.battery != null && v.battery < 15 ? 'var(--bad)' : undefined }}>{fmtPct(v.battery)}</div>
          </div>
        </div>
        <div className="row small" style={{ alignItems: 'flex-start' }}>
          <Icon name={g.state === 'none' ? 'map-pin-off' : 'map-pin'} size={18} className="muted" />
          <div>
            {g.state === 'none' && <span className="muted">ยังไม่พบตำแหน่ง</span>}
            {g.state !== 'none' && (
              <>
                <span className="bold">{g.state === 'fix' ? 'ตำแหน่งล่าสุด' : 'ตำแหน่งเก่า'}</span>{' '}
                <span className="num">{g.lat.toFixed(4)}, {g.lon.toFixed(4)}</span>
                <div className="tiny muted">ได้เมื่อ {fmtTime(g.fixAt)} ({fmtAgo(g.fixAt, v.at)})</div>
              </>
            )}
          </div>
        </div>
      </div>

      <div className="card stack">
        <div>
          <h3>ประวัติอุณหภูมิในอุปกรณ์</h3>
          <div className="tiny muted">อ่านผ่าน Bluetooth เพื่อแสดงในแอปเท่านั้น ข้อมูลเต็มทริปจะส่งขึ้น Server หลังจบทริป</div>
        </div>
        <TempChart trip={trip} current={v.temp} at={v.at} />
        <div className="row tiny muted wrap" style={{ gap: 12 }}>
          <span className="row" style={{ gap: 4 }}><span style={{ width: 14, height: 8, background: 'var(--ok-bg)', border: '1px solid #b9dfcb', borderRadius: 2 }} /> ช่วงที่อนุญาต</span>
          <span className="row" style={{ gap: 4 }}><span style={{ width: 14, height: 2, background: 'var(--teal)' }} /> อุณหภูมิ</span>
          <span className="row" style={{ gap: 4 }}><span style={{ width: 7, height: 7, borderRadius: 4, background: 'var(--bad)' }} /> นอกช่วง</span>
        </div>
      </div>

      <div className="card stack tight">
        <h3>เหตุการณ์ในทริป</h3>
        {[...trip.events].reverse().map((e, i) => (
          <div key={i} className="row small" style={{ alignItems: 'flex-start', padding: '4px 0' }}>
            <Icon
              name={isAlarm(e.type) ? 'alert-triangle' : e.type === 'ALARM_ACK' ? 'bell-check' : e.type === 'ALARM_CLEAR' ? 'circle-check' : 'flag'}
              size={16}
              style={{ color: isAlarm(e.type) ? 'var(--bad)' : e.type === 'ALARM_CLEAR' ? 'var(--ok)' : 'var(--muted)', marginTop: 2 }}
            />
            <div className="grow">
              <div style={{ fontWeight: isAlarm(e.type) ? 600 : 500 }}>{eventText(e)}</div>
              <div className="tiny muted">
                {fmtDateTime(e.t)}
                {e.temp != null ? ` · ${fmtTemp(e.temp)}` : ''}
              </div>
            </div>
          </div>
        ))}
        <div className="tiny muted">แจ้งเตือนทั้งทริป {trip.alarms.raised} ครั้ง</div>
      </div>

      <div style={{ position: 'sticky', bottom: -24, padding: '10px 0 14px', background: 'linear-gradient(to top, var(--bg) 75%, transparent)' }}>
        <Btn block icon="player-stop" onClick={() => nav.push({ name: 'stop-trip' })}>
          จบทริป
        </Btn>
      </div>
    </>
  );
}

export function TempChart({ trip, current, at }) {
  const W = 340;
  const H = 180;
  const P = { l: 34, r: 10, t: 10, b: 24 };
  const pts = [...trip.points];
  if (at > (pts[pts.length - 1]?.t ?? 0)) pts.push({ t: at, c: current, live: true });
  const vals = pts.map((p) => p.c).filter((c) => c != null);
  const lo = Math.floor(Math.min(trip.low - 1, ...vals) - 0.5);
  const hi = Math.ceil(Math.max(trip.high + 1, ...vals) + 0.5);
  const t0 = trip.startUtc;
  const t1 = Math.max(at, t0 + 60000);
  const x = (t) => P.l + ((t - t0) / (t1 - t0)) * (W - P.l - P.r);
  const y = (c) => P.t + ((hi - c) / (hi - lo)) * (H - P.t - P.b);
  let d = '';
  let pen = false;
  for (const p of pts) {
    if (p.c == null) {
      pen = false;
      continue;
    }
    d += `${pen ? 'L' : 'M'}${x(p.t).toFixed(1)},${y(p.c).toFixed(1)} `;
    pen = true;
  }
  const ticks = [];
  const step = hi - lo > 16 ? 5 : hi - lo > 8 ? 2 : 1;
  for (let c = Math.ceil(lo / step) * step; c <= hi; c += step) ticks.push(c);
  const out = pts.filter((p) => p.c != null && (p.c > trip.high || p.c < trip.low));
  const last = pts[pts.length - 1];
  const gaps = pts.filter((p) => p.c == null);
  return (
    <svg className="chart" viewBox={`0 0 ${W} ${H}`} role="img" aria-label={`กราฟอุณหภูมิ ช่วงที่อนุญาต ${trip.low} ถึง ${trip.high} องศา`}>
      <rect x={P.l} y={y(trip.high)} width={W - P.l - P.r} height={y(trip.low) - y(trip.high)} fill="#e2f3eb" />
      {ticks.map((c) => (
        <g key={c}>
          <line x1={P.l} x2={W - P.r} y1={y(c)} y2={y(c)} stroke="#e6edf1" strokeWidth="1" />
          <text x={P.l - 6} y={y(c) + 3} textAnchor="end">{c}°</text>
        </g>
      ))}
      <line x1={P.l} x2={W - P.r} y1={y(trip.high)} y2={y(trip.high)} stroke="#1d8a5a" strokeDasharray="4 3" strokeWidth="1" />
      <line x1={P.l} x2={W - P.r} y1={y(trip.low)} y2={y(trip.low)} stroke="#1d8a5a" strokeDasharray="4 3" strokeWidth="1" />
      {gaps.map((p, i) => (
        <rect key={i} x={x(p.t) - 1.5} y={P.t} width="3" height={H - P.t - P.b} fill="#f3d7a6" opacity="0.7" />
      ))}
      <path d={d} fill="none" stroke="#0e8391" strokeWidth="2" strokeLinejoin="round" />
      {out.map((p, i) => (
        <circle key={i} cx={x(p.t)} cy={y(p.c)} r="2.6" fill="#c63737" />
      ))}
      {last?.c != null && <circle cx={x(last.t)} cy={y(last.c)} r="4.5" fill="#fff" stroke={last.c > trip.high || last.c < trip.low ? '#c63737' : '#0e8391'} strokeWidth="2.5" />}
      <text x={P.l} y={H - 6}>{fmtTime(t0)}</text>
      <text x={W - P.r} y={H - 6} textAnchor="end">{fmtTime(t1)}</text>
    </svg>
  );
}
