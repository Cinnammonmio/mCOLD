import { useRef, useState } from 'react';
import { nav, useApp, simNow, update, userName } from '../store.js';
import { ackAlarm, BleLost, clearPendingCommand, deviceView, findDevice, inRange, isLinked, pendingCommand, pendingTrips, startTrip } from '../sim.js';
import { fmtAgo, fmtDateTime, fmtDuration, fmtNum, fmtPct, fmtPeriod, fmtTemp, fmtTime, tripLabel } from '../format.js';
import { requestConnect } from '../ConnectSheet.jsx';
import { Banner, BatteryIcon, Btn, Chip, Field, Icon } from '../ui.jsx';
import { ERR_TEXT, preflight } from './TripFlows.jsx';
import { toast } from '../store.js';
import { Freshness, NeedConnection } from './Mcold.jsx';

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
      <QuickStart dev={dev} v={v} linked={linked} />
      {last && (
        <div className="row small" style={{ gap: 6, padding: '0 2px' }}>
          <Icon name="history" size={16} className="muted" />
          <span className="grow muted" style={{ minWidth: 0, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
            ล่าสุด: {s.appTrips[last.trip_id]?.name || tripLabel(last)} · {fmtDuration(last.stopUtc - last.startUtc)}
          </span>
          {last.sent ? <Chip tone="ok">ส่งแล้ว</Chip> : <Chip tone="warn">รอส่ง</Chip>}
        </div>
      )}
      {pending > 0 && (
        <button className="row small" onClick={() => nav.toTab('send')} style={{ border: 0, background: 'var(--warn-bg)', color: 'var(--warn-ink)', borderRadius: 10, padding: '8px 10px', fontWeight: 600, width: '100%', textAlign: 'left' }}>
          <Icon name="cloud-upload" size={16} />
          <span className="grow">มีทริปรอส่ง {pending} ทริป (ข้อมูลยังอยู่ในอุปกรณ์)</span>
          ส่งข้อมูล <Icon name="chevron-right" size={16} />
        </button>
      )}
    </>
  );
}

const fix1 = (n) => Number(n).toFixed(1);

/**
 * Start a trip on one screen: min/max are already filled (the box's last trip, else 2–8 °C),
 * the readiness check runs on its own, and one tap sends START_TRIP.
 */
function QuickStart({ dev, v, linked }) {
  const s = useApp();
  const pend = pendingCommand(s, dev.sn, 'START');
  const prev = dev.trips[dev.trips.length - 1];
  const [form, setForm] = useState(() => ({
    low: fix1(pend?.args.low ?? prev?.low ?? 2),
    high: fix1(pend?.args.high ?? prev?.high ?? 8),
    name: '',
  }));
  const [busy, setBusy] = useState(false);
  const [result, setResult] = useState(pend ? { kind: 'unknown' } : null);
  const guard = useRef(false);
  const pf = preflight(s, dev);
  const blocks = pf.checks.filter((c) => c.state === 'block');
  const warns = pf.checks.filter((c) => c.state === 'warn');

  const low = parseFloat(form.low);
  const high = parseFloat(form.high);
  const errs = {};
  if (form.low.trim() === '' || !Number.isFinite(low)) errs.low = 'กรอกตัวเลข';
  if (form.high.trim() === '' || !Number.isFinite(high)) errs.high = 'กรอกตัวเลข';
  if (!errs.low && !errs.high && low >= high) errs.high = 'ต้องมากกว่าค่าต่ำสุด';
  if (!errs.low && (low < -40 || low > 100)) errs.low = 'ตั้งได้ −40 ถึง 100°C';
  if (!errs.high && (high < -40 || high > 100)) errs.high = 'ตั้งได้ −40 ถึง 100°C';
  const formOk = Object.keys(errs).length === 0;

  const start = async () => {
    if (guard.current) return; // one tap = one command, even if pressed twice
    guard.current = true;
    setBusy(true);
    setResult(null);
    try {
      const res = await startTrip(dev.sn, { low, high, name: form.name.trim() });
      if (res.ok) {
        toast('เริ่มทริปแล้ว อุปกรณ์ยืนยันการเริ่มบันทึก', 'ok');
        const c = res.clock;
        if (c?.adjusted && Math.abs(c.diff_s) > 5) setTimeout(() => toast(`ตั้งเวลาของกล่องให้ตรงกับมือถือแล้ว (เดิมต่างกัน ${Math.abs(c.diff_s)} วินาที)`, 'info'), 600);
      } else setResult({ kind: 'refused', err: res.err });
    } catch (e) {
      if (e instanceof BleLost) setResult({ kind: e.when === 'after' ? 'unknown' : 'lost' });
      else throw e;
    } finally {
      guard.current = false;
      setBusy(false);
    }
  };
  const reconnectAndRetry = async () => {
    const ok = await requestConnect(dev.sn, 'เชื่อมต่อใหม่เพื่อตรวจสอบว่าอุปกรณ์เริ่มทริปแล้วหรือยัง');
    if (ok) start();
  };

  const field = (key, label) => (
    <Field label={label} htmlFor={`qs-${key}`} error={errs[key]}>
      <div className="input-unit">
        <input
          id={`qs-${key}`}
          className={`input num ${errs[key] ? 'invalid' : ''}`}
          inputMode="decimal"
          value={form[key]}
          disabled={busy || result?.kind === 'unknown'}
          onChange={(e) => setForm({ ...form, [key]: e.target.value })}
          style={{ fontSize: 22, fontWeight: 600, minHeight: 56 }}
        />
        <span className="unit">°C</span>
      </div>
    </Field>
  );

  return (
    <div className="card raised stack">
      <div className="row between">
        <h3>เริ่มทริป</h3>
        <span className="tiny muted">ไม่มีทริปทำงาน</span>
      </div>
      <div className="grid2">
        {field('low', 'ต่ำสุด')}
        {field('high', 'สูงสุด')}
      </div>
      {formOk && low < 0 && <div className="tiny" style={{ color: 'var(--warn)' }}>หัววัดรุ่นนี้ยังไม่ยืนยันการวัดต่ำกว่า 0°C</div>}
      <input
        id="qs-name"
        className="input"
        aria-label="กล่องที่ติด"
        maxLength={40}
        placeholder="กล่องที่ติด (ไม่บังคับ) เช่น กล่องวัคซีน #12"
        value={form.name}
        disabled={busy || result?.kind === 'unknown'}
        onChange={(e) => setForm({ ...form, name: e.target.value })}
      />
      <div className="tiny muted">
        บันทึกทุก {fmtPeriod(dev.config.sample_period_s)} · เตือนเมื่อนอกช่วง{dev.config.alarm_dwell_s ? `นาน ${fmtPeriod(dev.config.alarm_dwell_s)}` : 'ทันที'}
      </div>

      {linked && blocks.length > 0 && (
        <Banner tone="bad" title="ยังเริ่มทริปไม่ได้">
          {blocks.map((c) => <div key={c.key}>{c.title}: {c.note}</div>)}
        </Banner>
      )}
      {linked && !blocks.length && (
        <div className="tiny" style={{ color: warns.length ? 'var(--warn)' : 'var(--ok)' }}>
          {warns.length ? warns.map((c) => `${c.title}: ${c.note}`).join(' · ') : `พร้อม · แบต ${fmtPct(v.battery)} · หัววัดปกติ · พื้นที่ใช้ไป ${fmtPct(v.storagePct)}`}
        </div>
      )}
      {result?.kind === 'refused' && <Banner tone="bad" title={ERR_TEXT[result.err]?.title || 'อุปกรณ์ไม่เริ่มทริป'}>{ERR_TEXT[result.err]?.fix}</Banner>}
      {result?.kind === 'lost' && <Banner tone="bad" title="การเชื่อมต่อหลุดก่อนส่งคำสั่ง">อุปกรณ์ยังไม่ได้รับคำสั่ง ทริปยังไม่เริ่ม</Banner>}
      {result?.kind === 'unknown' && (
        <Banner tone="warn" title="ไม่ได้รับการยืนยันจากอุปกรณ์">
          ยังไม่ทราบว่าทริปเริ่มแล้วหรือไม่ เชื่อมต่อใหม่เพื่อตรวจสอบ ระบบจะไม่สร้างทริปซ้ำ
        </Banner>
      )}

      {result?.kind === 'unknown' || result?.kind === 'lost' ? (
        <>
          <Btn kind="teal" block icon="nfc" onClick={reconnectAndRetry} style={{ minHeight: 56 }}>
            เชื่อมต่อและลองอีกครั้ง
          </Btn>
          {result.kind === 'unknown' && (
            <Btn kind="ghost" onClick={() => { clearPendingCommand(dev.sn); setResult(null); }} style={{ alignSelf: 'center' }}>
              ยกเลิก
            </Btn>
          )}
        </>
      ) : !linked ? (
        <Btn kind="teal" block icon="nfc" onClick={() => requestConnect(dev.sn)} style={{ minHeight: 56 }}>
          แตะ NFC เพื่อเชื่อมต่อ
        </Btn>
      ) : (
        <Btn kind="teal" block icon="player-play" busy={busy} disabled={!formOk || blocks.length > 0} onClick={start} style={{ minHeight: 56, fontSize: 17 }}>
          {busy ? 'กำลังเริ่มทริป…' : `เริ่มทริป ${form.low}–${form.high}°C`}
        </Btn>
      )}
    </div>
  );
}

function AlarmBanner({ dev, v, linked }) {
  const s = useApp();
  const a = v.trip.alarms;
  const acks = s.appTrips[v.trip.trip_id]?.acks || [];
  const lastAck = acks[acks.length - 1];
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
    <div className="banner bad" role="alert" style={{ border: a.acked ? '1px solid #f0c8c8' : 0, padding: '9px 10px', alignItems: 'center' }}>
      <Icon name="bell-ringing" />
      <div className="grow small" style={{ minWidth: 0 }}>
        <b>{titles}</b>
        <div>
          ตั้งแต่ {fmtTime(a.last?.t)}
          {a.last?.temp != null ? ` · ${fmtTemp(a.last.temp)}` : ''}
          {a.acked && ` · รับทราบแล้ว${lastAck ? ` (${userName(s, lastAck.by)})` : ''} ปัญหายังไม่หาย`}
        </div>
      </div>
      {!a.acked &&
        (linked ? (
          <Btn size="sm" kind="danger" busy={busy} icon="bell-check" onClick={ack} title="การรับทราบไม่ลบประวัติ และการแจ้งเตือนยังอยู่จนอุณหภูมิกลับเข้าช่วง">
            รับทราบ
          </Btn>
        ) : (
          <Btn size="sm" kind="secondary" icon="nfc" onClick={() => requestConnect(dev.sn, 'เชื่อมต่อเพื่อรับทราบการแจ้งเตือน')}>
            เชื่อมต่อ
          </Btn>
        ))}
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
  const [showEvents, setShowEvents] = useState(false);
  const lastEv = trip.events[trip.events.length - 1];
  const stat = (label, value, bad) => (
    <div className="metric" style={{ padding: '6px 8px' }}>
      <div className="label" style={{ fontSize: 11 }}>{label}</div>
      <div className="bold num" style={{ fontSize: 14, color: bad ? 'var(--bad)' : undefined, whiteSpace: 'nowrap' }}>{value}</div>
    </div>
  );
  return (
    <>
      {!linked && (
        <div className="row small" style={{ background: 'var(--soft)', borderRadius: 10, padding: '6px 10px' }}>
          <Icon name="history" size={16} className="muted" />
          <span className="grow">ข้อมูลล่าสุดเมื่อ {fmtTime(v.at)} ({fmtAgo(v.at, now)})</span>
          <Btn size="sm" kind="teal" icon="nfc" onClick={() => requestConnect(dev.sn)}>เชื่อมต่อ</Btn>
        </div>
      )}
      <AlarmBanner dev={dev} v={v} linked={linked} />

      <div className="card raised stack" style={{ gap: 8, padding: 12 }}>
        <div className="row between" style={{ alignItems: 'flex-start' }}>
          <div className="grow" style={{ minWidth: 0 }}>
            <div className="bold" style={{ fontSize: 16 }}>{meta?.name || tripLabel(trip)}</div>
            <div className="tiny muted">
              {meta?.name ? `${tripLabel(trip)} · ` : ''}เริ่ม {fmtDateTime(trip.startUtc)}{meta?.startedBy ? ` · ${userName(s, meta.startedBy.by)}` : ''}
            </div>
          </div>
          <Chip tone="info" dot>{fmtDuration(v.at - trip.startUtc)}</Chip>
        </div>
        <div className="row between" style={{ alignItems: 'center' }}>
          <div>
            <span className={`tempbig ${linked ? '' : 'stale'}`} style={{ color: ok === false ? 'var(--bad)' : undefined, fontSize: 34 }}>
              {v.temp == null ? 'ไม่พบหัววัด' : fmtTemp(v.temp, 2)}
            </span>
            <span className="tiny muted"> วัดเมื่อ {fmtTime(v.at)}</span>
          </div>
          {ok === true && <Chip tone="ok" icon="check">อยู่ในช่วง</Chip>}
          {ok === false && <Chip tone="bad" icon="alert-triangle">นอกช่วง</Chip>}
          {ok === null && <Chip tone="warn">ไม่มีค่า</Chip>}
        </div>
        <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr 0.7fr', gap: 6 }}>
          {stat('ช่วงที่อนุญาต', `${fmtTemp(trip.low)}–${fmtTemp(trip.high)}`)}
          {stat('ต่ำ / สูงที่บันทึก', `${fmtTemp(trip.min)} / ${fmtTemp(trip.max)}`, (trip.min != null && trip.min < trip.low) || (trip.max != null && trip.max > trip.high))}
          {stat('แบต', fmtPct(v.battery), v.battery != null && v.battery < 15)}
        </div>
        <div className="tiny muted row wrap" style={{ gap: 10 }}>
          <span className="row" style={{ gap: 3 }}><Icon name="database" size={13} />{fmtNum(trip.rows)} แถว · ทุก {fmtPeriod(trip.period)}</span>
          <span className="row" style={{ gap: 3 }}>
            <Icon name={g.state === 'none' ? 'map-pin-off' : 'map-pin'} size={13} />
            {g.state === 'none' ? 'ยังไม่พบตำแหน่ง' : `${g.lat.toFixed(4)}, ${g.lon.toFixed(4)} (${fmtAgo(g.fixAt, v.at)})`}
          </span>
        </div>
        <TempChart trip={trip} current={v.temp} at={v.at} compact />
        <div className="tiny muted">กราฟจากข้อมูลในอุปกรณ์ผ่าน Bluetooth · ข้อมูลเต็มทริปส่ง Server หลังจบทริป</div>
      </div>

      <div className="card stack tight" style={{ padding: '10px 12px' }}>
        <button className="row between" onClick={() => setShowEvents(!showEvents)} aria-expanded={showEvents} style={{ border: 0, background: 'none', padding: 0, width: '100%', textAlign: 'left' }}>
          <span className="small grow" style={{ minWidth: 0, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
            <b>เหตุการณ์ {trip.events.length}</b> <span className="muted">· แจ้งเตือน {trip.alarms.raised} ครั้ง · ล่าสุด {eventText(lastEv)} {fmtTime(lastEv.t)}</span>
          </span>
          <Icon name="chevron-down" size={18} style={{ transform: showEvents ? 'rotate(180deg)' : 'none', flex: 'none' }} />
        </button>
        {showEvents && [...trip.events].reverse().map((e, i) => (
          <div key={i} className="row small" style={{ alignItems: 'flex-start', padding: '3px 0' }}>
            <Icon
              name={isAlarm(e.type) ? 'alert-triangle' : e.type === 'ALARM_ACK' ? 'bell-check' : e.type === 'ALARM_CLEAR' ? 'circle-check' : 'flag'}
              size={16}
              style={{ color: isAlarm(e.type) ? 'var(--bad)' : e.type === 'ALARM_CLEAR' ? 'var(--ok)' : 'var(--muted)', marginTop: 2 }}
            />
            <div className="grow">
              <div style={{ fontWeight: isAlarm(e.type) ? 600 : 500 }}>
                {eventText(e)}
                {e.type === 'ALARM_ACK' && meta?.acks?.find((k) => Math.abs(k.t - e.t) < 1000) && (
                  <span className="muted" style={{ fontWeight: 400 }}> · {userName(s, meta.acks.find((k) => Math.abs(k.t - e.t) < 1000).by)}</span>
                )}
                {e.type === 'TRIP_START' && meta?.startedBy && <span className="muted" style={{ fontWeight: 400 }}> · {userName(s, meta.startedBy.by)}</span>}
              </div>
              <div className="tiny muted">
                {fmtDateTime(e.t)}
                {e.temp != null ? ` · ${fmtTemp(e.temp)}` : ''}
              </div>
            </div>
          </div>
        ))}
      </div>

      <div style={{ position: 'sticky', bottom: -24, padding: '6px 0 14px', background: 'linear-gradient(to top, var(--bg) 75%, transparent)' }}>
        <Btn block icon="player-stop" onClick={() => nav.push({ name: 'stop-trip' })}>
          จบทริป
        </Btn>
      </div>
    </>
  );
}

export function TempChart({ trip, current, at, compact }) {
  const W = 340;
  const H = compact ? 120 : 180;
  const P = { l: 34, r: 10, t: 10, b: 24 };
  const pts = [...trip.points];
  // the live reading (undefined = a stored trip with no live point)
  if (current !== undefined && at > (pts[pts.length - 1]?.t ?? 0)) pts.push({ t: at, c: current, live: true });
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
      <rect x={P.l} y={y(trip.high)} width={W - P.l - P.r} height={y(trip.low) - y(trip.high)} style={{ fill: 'var(--ok-bg)' }} />
      {ticks.map((c) => (
        <g key={c}>
          <line x1={P.l} x2={W - P.r} y1={y(c)} y2={y(c)} style={{ stroke: 'var(--line)' }} strokeWidth="1" />
          <text x={P.l - 6} y={y(c) + 3} textAnchor="end">{c}°</text>
        </g>
      ))}
      <line x1={P.l} x2={W - P.r} y1={y(trip.high)} y2={y(trip.high)} style={{ stroke: 'var(--ok)' }} strokeDasharray="4 3" strokeWidth="1" />
      <line x1={P.l} x2={W - P.r} y1={y(trip.low)} y2={y(trip.low)} style={{ stroke: 'var(--ok)' }} strokeDasharray="4 3" strokeWidth="1" />
      {gaps.map((p, i) => (
        <rect key={i} x={x(p.t) - 1.5} y={P.t} width="3" height={H - P.t - P.b} style={{ fill: 'var(--gap)' }} opacity="0.7" />
      ))}
      <path d={d} fill="none" style={{ stroke: 'var(--teal)' }} strokeWidth="2" strokeLinejoin="round" />
      {out.map((p, i) => (
        <circle key={i} cx={x(p.t)} cy={y(p.c)} r="2.6" style={{ fill: 'var(--bad)' }} />
      ))}
      {last?.c != null && <circle cx={x(last.t)} cy={y(last.c)} r="4.5" style={{ fill: 'var(--surface)', stroke: last.c > trip.high || last.c < trip.low ? 'var(--bad)' : 'var(--teal)' }} strokeWidth="2.5" />}
      <text x={P.l} y={H - 6}>{fmtTime(t0)}</text>
      <text x={W - P.r} y={H - 6} textAnchor="end">{fmtTime(t1)}</text>
    </svg>
  );
}
