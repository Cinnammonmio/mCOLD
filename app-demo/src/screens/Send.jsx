// Select finished trips and send them: box → BLE → app → phone internet → server → tell the box.
import { useState } from 'react';
import { nav, useApp, userName } from '../store.js';
import { csvBytes, deviceView, findDevice, isLinked, runBatch, transferSteps, tripDelivery, tripSeries } from '../sim.js';
import { fmtBytes, fmtDateTime, fmtDuration, fmtNum, fmtTime, tripLabel } from '../format.js';
import { requestConnect } from '../ConnectSheet.jsx';
import { AppBar, Banner, Btn, Chip, Icon, Seg, Steps } from '../ui.jsx';
import { DeviceContext, NeedConnection } from './Mcold.jsx';

const NEEDS_BLE = ['queued', 'receiving', 'ble_interrupted', 'server_ok', 'mark_failed', undefined];

function PathStrip() {
  const node = (icon, label) => (
    <div className="stack tight center" style={{ alignItems: 'center', gap: 2, minWidth: 0 }}>
      <span className="dev-icon" style={{ width: 34, height: 34, borderRadius: 10 }}><Icon name={icon} size={18} /></span>
      <span className="tiny muted">{label}</span>
    </div>
  );
  const arrow = <Icon name="arrow-right" size={14} className="muted" style={{ marginBottom: 16 }} />;
  return (
    <div className="card row between" style={{ padding: '10px 8px' }} aria-label="เส้นทางข้อมูล: อุปกรณ์ ผ่าน Bluetooth ไปยังแอป แล้วผ่านอินเทอร์เน็ตไปยัง Server">
      {node('package', 'อุปกรณ์')}
      {arrow}
      {node('bluetooth', 'Bluetooth')}
      {arrow}
      {node('device-mobile', 'แอป')}
      {arrow}
      {node('world', 'อินเทอร์เน็ต')}
      {arrow}
      {node('server', 'Server')}
    </div>
  );
}

export function SendTab() {
  const s = useApp();
  const dev = findDevice(s, s.selectedSn);
  const v = deviceView(s, dev);
  const linked = isLinked(s, dev.sn);
  const [filter, setFilter] = useState('pending');
  const [sel, setSel] = useState([]);

  const trips = [...dev.trips].reverse().map((t) => ({ t, d: tripDelivery(s, dev, t) }));
  const counts = {
    all: trips.length,
    pending: trips.filter((x) => x.d.pending).length,
    sent: trips.filter((x) => x.d.key === 'sent').length,
  };
  const shown = trips.filter((x) => filter === 'all' || (filter === 'pending' ? x.d.pending || x.d.key === 'active' : x.d.key === 'sent'));
  const selectable = trips.filter((x) => x.d.pending).map((x) => x.t.trip_id);
  const selected = sel.filter((id) => selectable.includes(id));
  // Internet the phone will use: the CSV of each trip (receiving over Bluetooth uses none).
  const sizeOf = (id) => csvBytes(dev, dev.trips.find((t) => t.trip_id === id));
  const selBytes = selected.reduce((n, id) => n + sizeOf(id), 0);
  const allOn = selectable.length > 0 && selected.length === selectable.length;
  const toggle = (id) => setSel(selected.includes(id) ? selected.filter((x) => x !== id) : [...selected, id]);
  const running = s.batch?.running;

  const send = async () => {
    if (!selected.length || running) return;
    const needBle = selected.some((id) => NEEDS_BLE.includes(s.transfers[id]?.stage));
    if (needBle && !linked) {
      const ok = await requestConnect(dev.sn, 'เชื่อมต่อเพื่อรับข้อมูลทริปจากอุปกรณ์');
      if (!ok) return;
    }
    const ids = [...selected].reverse(); // oldest first, one at a time
    setSel([]);
    nav.push({ name: 'send-progress', ids });
    runBatch(ids);
  };

  return (
    <>
      <DeviceContext />
      {!v.known ? (
        <NeedConnection dev={dev} text="แอปยังไม่เคยอ่านรายการทริปจากเครื่องนี้" reason="เชื่อมต่อเพื่ออ่านรายการทริป" />
      ) : (
        <>
          <PathStrip />
          {!linked && (
            <Banner tone="neutral" icon="history" title={`รายการจากการเชื่อมต่อล่าสุด ${fmtTime(v.at)}`}>
              เลือกทริปได้เลย แอปจะขอให้แตะ NFC ก่อนรับข้อมูลจากอุปกรณ์
            </Banner>
          )}
          {!s.internet && (
            <Banner tone="warn" icon="world-off" title="มือถือไม่มีอินเทอร์เน็ต">
              ยังรับข้อมูลจากอุปกรณ์เข้าแอปได้ แอปจะเก็บไว้และส่งขึ้น Server เมื่อมีอินเทอร์เน็ต
            </Banner>
          )}
          <Seg
            label="ตัวกรองทริป"
            value={filter}
            onChange={setFilter}
            options={[
              { value: 'all', label: `ทั้งหมด ${counts.all}` },
              { value: 'pending', label: `รอส่ง ${counts.pending}` },
              { value: 'sent', label: `ส่งแล้ว ${counts.sent}` },
            ]}
          />
          {filter !== 'sent' && selectable.length > 0 && (
            <div className="row between small" style={{ padding: '0 2px', alignItems: 'flex-start' }}>
              <label className="check">
                <input type="checkbox" checked={allOn} onChange={() => setSel(allOn ? [] : selectable)} />
                <span>เลือกทั้งหมดที่รอส่ง ({selectable.length})</span>
              </label>
              {/* total of what is selected right now */}
              <span className="bold" style={{ whiteSpace: 'nowrap', paddingTop: 2 }}>
                {selected.length ? `เลือก ${selected.length} · ใช้เน็ต ≈ ${fmtBytes(selBytes)}` : <span className="muted" style={{ fontWeight: 400 }}>ยังไม่ได้เลือก</span>}
              </span>
            </div>
          )}
          {shown.length === 0 && (
            <div className="card center small muted" style={{ padding: 24 }}>
              {filter === 'pending' ? 'ไม่มีทริปที่รอส่ง' : filter === 'sent' ? 'ยังไม่มีทริปที่ส่งแล้ว' : 'ยังไม่มีทริปในอุปกรณ์นี้'}
            </div>
          )}
          <div className="stack tight">
            {shown.map(({ t, d }) => (
              <TripRow key={t.trip_id} dev={dev} trip={t} d={d} checked={selected.includes(t.trip_id)} onToggle={() => toggle(t.trip_id)} />
            ))}
          </div>
          <div className="tiny muted" style={{ padding: '0 2px' }}>
            การส่งไม่ลบทริปออกจากอุปกรณ์ และส่งซ้ำได้โดยข้อมูลบน Server ไม่ซ้ำ
          </div>
          <div className="tiny muted" style={{ padding: '0 2px' }}>
            ขนาดเป็นค่าประมาณของไฟล์ CSV ที่ส่งด้วยอินเทอร์เน็ตมือถือ การรับข้อมูลจากอุปกรณ์ผ่าน Bluetooth ไม่ใช้อินเทอร์เน็ต ถ้าส่งไม่สำเร็จแล้วลองใหม่จะใช้อีกครั้ง
          </div>
          <div style={{ position: 'sticky', bottom: -24, padding: '10px 0 14px', background: 'linear-gradient(to top, var(--bg) 75%, transparent)' }}>
            <Btn kind="teal" block icon="cloud-upload" disabled={!selected.length || running} onClick={send}>
              {running ? 'กำลังส่งรายการก่อนหน้า…' : selected.length ? `ส่งที่เลือก ${selected.length} ทริป · ใช้เน็ต ≈ ${fmtBytes(selBytes)}` : 'เลือกทริปที่ต้องการส่ง'}
            </Btn>
          </div>
        </>
      )}
    </>
  );
}

function TripRow({ dev, trip, d, checked, onToggle }) {
  const s = useApp();
  const active = d.key === 'active';
  const end = trip.stopUtc;
  const rows = end ? tripSeries(dev, trip, end).rows : null;
  const name = s.appTrips[trip.trip_id]?.name;
  const disabled = !d.pending;
  return (
    <label className={`card row ${checked ? 'selected' : ''}`} style={{ alignItems: 'flex-start', cursor: disabled ? 'default' : 'pointer', opacity: d.key === 'sent' ? 0.85 : 1 }}>
      <input type="checkbox" checked={checked} disabled={disabled} onChange={onToggle} style={{ width: 20, height: 20, marginTop: 2, accentColor: 'var(--teal)', flex: 'none' }} aria-label={`เลือก ${tripLabel(trip)}`} />
      <div className="grow stack tight" style={{ gap: 4 }}>
        <div className="row between" style={{ alignItems: 'flex-start' }}>
          <span className="bold">{name || tripLabel(trip)}</span>
        </div>
        {name && <span className="tiny muted">{tripLabel(trip)}</span>}
        <span className="small muted">
          {fmtDateTime(trip.startUtc)} – {end ? fmtDateTime(end) : 'ยังบันทึกอยู่'}
        </span>
        {(s.appTrips[trip.trip_id]?.startedBy || s.appTrips[trip.trip_id]?.stoppedBy) && (
          <span className="tiny muted">
            {s.appTrips[trip.trip_id]?.startedBy && <>เริ่มโดย {userName(s, s.appTrips[trip.trip_id].startedBy.by)}</>}
            {s.appTrips[trip.trip_id]?.stoppedBy && <> · จบโดย {userName(s, s.appTrips[trip.trip_id].stoppedBy.by)}</>}
          </span>
        )}
        <div className="row wrap small" style={{ gap: 10 }}>
          <span className="row" style={{ gap: 4 }}><Icon name="clock" size={14} className="muted" />{end ? fmtDuration(end - trip.startUtc) : '—'}</span>
          <span className="row" style={{ gap: 4 }}><Icon name="database" size={14} className="muted" />{rows != null ? `${fmtNum(rows)} แถว` : '—'}</span>
          {end && <span className="row" style={{ gap: 4 }}><Icon name="world" size={14} className="muted" />{d.key === 'sent' ? fmtBytes(csvBytes(dev, trip)) : `ใช้เน็ต ≈ ${fmtBytes(csvBytes(dev, trip))}`}</span>}
        </div>
        <div><Chip tone={d.tone} dot={active}>{d.label}</Chip></div>
        {active && (
          <div className="tiny muted">
            ทริปยังทำงานอยู่ ส่งข้อมูลเต็มทริปได้หลังจบทริป{' '}
            <a href="#trip" onClick={(e) => { e.preventDefault(); nav.toTab('trip'); }} style={{ color: 'var(--teal)', fontWeight: 600 }}>ไปที่ทริป</a>
          </div>
        )}
      </div>
    </label>
  );
}

const STEP_TEXT = {
  ble: 'รับข้อมูลจากอุปกรณ์ผ่าน Bluetooth',
  csv: 'เตรียมไฟล์ข้อมูล CSV ในแอป',
  upload: 'ส่งข้อมูลขึ้น Server',
  server: 'รอ Server ยืนยันว่าเก็บข้อมูลแล้ว',
  device: 'แจ้งอุปกรณ์ว่าทริปนี้ส่งสำเร็จแล้ว',
};

export function SendProgress({ ids }) {
  const s = useApp();
  const running = s.batch?.running;
  const items = ids
    .map((id) => {
      const dev = s.devices.find((d) => d.trips.some((t) => t.trip_id === id));
      const trip = dev?.trips.find((t) => t.trip_id === id);
      return dev && trip ? { id, dev, trip, tr: s.transfers[id] } : null;
    })
    .filter(Boolean);
  const done = items.filter((x) => x.trip.sent).length;
  const left = items.filter((x) => !x.trip.sent);

  const retry = async (only) => {
    if (running) return;
    const list = only ? left.filter((x) => x.id === only) : left;
    if (!list.length) return;
    const needBle = list.filter((x) => NEEDS_BLE.includes(x.tr?.stage) && !isLinked(s, x.dev.sn));
    for (const x of needBle) {
      // eslint-disable-next-line no-await-in-loop
      const ok = await requestConnect(x.dev.sn, x.tr?.stage === 'ble_interrupted' ? 'เชื่อมต่อเพื่อรับข้อมูลต่อจากเดิม' : 'เชื่อมต่อเพื่อยืนยันกับอุปกรณ์ว่าส่งสำเร็จแล้ว');
      if (!ok) return;
      break; // one box at a time in this prototype
    }
    runBatch(list.map((x) => x.id));
  };

  return (
    <div className="screen">
      <AppBar title="ส่งข้อมูล" sub={running ? 'กำลังทำงานทีละทริป' : `${items.length} ทริป`} onBack={() => nav.toTab('send')} backLabel="รายการ" />
      <div className="content stack with-footer">
        <div className="card row between">
          <div>
            <div className="tiny muted">ส่งสำเร็จครบทุกขั้น</div>
            <div className="bold" style={{ fontSize: 20 }}>{done} / {items.length} ทริป</div>
            <div className="tiny muted">รวมใช้เน็ตประมาณ {fmtBytes(items.reduce((n, x) => n + csvBytes(x.dev, x.trip), 0))}</div>
          </div>
          {running ? <Chip tone="info" icon="loader-2">กำลังทำงาน</Chip> : done === items.length ? <Chip tone="ok" icon="circle-check">เสร็จสิ้น</Chip> : <Chip tone="warn">ค้าง {left.length}</Chip>}
        </div>
        {!s.internet && (
          <Banner tone="warn" icon="world-off">มือถือไม่มีอินเทอร์เน็ต ข้อมูลที่รับครบแล้วจะรออยู่ในแอป และส่งอัตโนมัติเมื่อมีอินเทอร์เน็ต</Banner>
        )}
        {items.map((x) => (
          <TransferCard key={x.id} {...x} running={running} onRetry={() => retry(x.id)} />
        ))}
      </div>
      <div className="footer-bar stack tight">
        {running ? (
          <Btn block busy>กำลังส่ง…</Btn>
        ) : left.length ? (
          <>
            <Btn kind="teal" block icon="refresh" onClick={() => retry()}>
              ลองใหม่เฉพาะรายการที่ค้าง ({left.length})
            </Btn>
            <Btn kind="secondary" block onClick={() => nav.toTab('send')}>กลับไปที่รายการ</Btn>
          </>
        ) : (
          <Btn kind="teal" block icon="check" onClick={() => nav.toTab('send')}>เสร็จสิ้น</Btn>
        )}
      </div>
    </div>
  );
}

function TransferCard({ dev, trip, tr, running, onRetry }) {
  const s = useApp();
  const steps = trip.sent ? Object.fromEntries(['ble', 'csv', 'upload', 'server', 'device'].map((k) => [k, 'done'])) : transferSteps(tr);
  const stage = tr?.stage;
  const name = s.appTrips[trip.trip_id]?.name;
  const recvPct = tr ? Math.floor((100 * tr.rowsReceived) / Math.max(1, tr.totalRows)) : 0;
  const inApp = steps.ble === 'done';
  const atServer = steps.server === 'done';
  const items = ['ble', 'csv', 'upload', 'server', 'device'].map((k) => {
    let sub = null;
    let extra = null;
    if (k === 'ble' && tr) {
      sub = `${fmtNum(tr.rowsReceived)} / ${fmtNum(tr.totalRows)} แถว`;
      if (steps.ble === 'active' || steps.ble === 'error') extra = <div className={`progress ${steps.ble === 'error' ? 'warn' : ''}`}><div style={{ width: `${recvPct}%` }} /></div>;
    }
    if (k === 'csv' && tr?.csv) sub = tr.csv;
    if (k === 'upload' && steps.upload === 'wait') sub = 'รออินเทอร์เน็ต · ข้อมูลอยู่ในแอปแล้ว';
    if (k === 'upload' && steps.upload === 'error') sub = 'Server ไม่ยืนยัน · ข้อมูลยังอยู่ในแอป';
    if (k === 'device' && steps.device === 'wait') sub = 'Server รับแล้ว รอยืนยันกับอุปกรณ์';
    return { key: k, title: STEP_TEXT[k], sub, extra, state: steps[k] };
  });

  let action = null;
  if (!running && !trip.sent) {
    if (stage === 'ble_interrupted')
      action = (
        <Banner tone="warn" title="Bluetooth หลุดระหว่างรับข้อมูล" actions={<Btn size="sm" kind="teal" icon="nfc" onClick={onRetry}>เชื่อมต่อเพื่อรับต่อ</Btn>}>
          แอปเก็บข้อมูลที่รับแล้ว {recvPct}% จะรับต่อจากจุดเดิมโดยไม่ซ้ำ
        </Banner>
      );
    else if (stage === 'waiting_internet')
      action = (
        <Banner tone="warn" icon="world-off" title="รับเข้าแอปครบแล้ว · รออินเทอร์เน็ต" actions={<Btn size="sm" kind="secondary" icon="refresh" onClick={onRetry}>ลองส่งตอนนี้</Btn>}>
          ข้อมูลอยู่ในคิวของแอป จะส่งอัตโนมัติเมื่อมือถือมีอินเทอร์เน็ต
        </Banner>
      );
    else if (stage === 'upload_failed')
      action = (
        <Banner tone="bad" title="ส่งขึ้น Server ไม่สำเร็จ" actions={<Btn size="sm" kind="teal" icon="refresh" onClick={onRetry}>ลองส่งใหม่</Btn>}>
          ข้อมูลยังอยู่ในแอปครบ ไม่ต้องรับจากอุปกรณ์ใหม่
        </Banner>
      );
    else if (stage === 'mark_failed')
      action = (
        <Banner tone="warn" title="Server รับแล้ว รอยืนยันกับอุปกรณ์" actions={<Btn size="sm" kind="teal" icon="nfc" onClick={onRetry}>ยืนยันกับอุปกรณ์อีกครั้ง</Btn>}>
          ข้อมูลถึง Server แล้ว แต่ยังแจ้งอุปกรณ์ไม่สำเร็จ อุปกรณ์จึงยังแสดงว่ารอส่ง
        </Banner>
      );
    else if (!tr || stage === 'queued')
      action = <Banner tone="neutral" actions={<Btn size="sm" kind="teal" onClick={onRetry}>เริ่มส่ง</Btn>}>ยังไม่ได้เริ่ม</Banner>;
  }

  return (
    <div className="card stack">
      <div className="row between" style={{ alignItems: 'flex-start' }}>
        <div className="grow">
          <div className="bold">{name || tripLabel(trip)}</div>
          <div className="tiny muted">{dev.name} · {fmtDuration(trip.stopUtc - trip.startUtc)}</div>
        </div>
        {trip.sent ? <Chip tone="ok" icon="circle-check">ส่งแล้ว</Chip> : null}
      </div>
      <div className="row wrap" style={{ gap: 6 }}>
        <Chip tone={inApp ? 'info' : ''} icon={inApp ? 'device-mobile' : undefined}>{inApp ? 'รับเข้าแอปครบแล้ว' : 'ยังรับเข้าแอปไม่ครบ'}</Chip>
        <Chip tone={atServer ? 'ok' : ''} icon={atServer ? 'cloud-check' : undefined}>{atServer ? 'ส่งถึง Server สำเร็จแล้ว' : 'ยังไม่ถึง Server'}</Chip>
      </div>
      <Steps items={items} />
      {action}
      <div className="tiny muted">
        ไฟล์ CSV ประมาณ {fmtBytes(csvBytes(dev, trip))}
        {tr?.uploads ? ` · ใช้เน็ตไปแล้วประมาณ ${fmtBytes(csvBytes(dev, trip) * tr.uploads)}` : ''}
      </div>
      {tr?.sentBy && trip.sent && <div className="tiny muted">ส่งโดย {userName(s, tr.sentBy)} · {fmtTime(tr.doneAt)}</div>}
      {tr?.uploads > 1 && <div className="tiny muted">ส่งขึ้น Server แล้ว {tr.uploads} ครั้ง · การส่งซ้ำไม่ทำให้ข้อมูลซ้ำ</div>}
    </div>
  );
}
