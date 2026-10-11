import { nav, update, useApp } from '../store.js';
import { pendingTrips } from '../sim.js';
import { AppBar, Btn, Chip, Icon } from '../ui.jsx';

export const APP_NAME = 'Cold Chain Hub';

const PRODUCTS = [
  { id: 'etemp', name: 'eTEMP', desc: 'เครื่องวัดอุณหภูมิตู้เย็นและห้องเก็บยา', color: '#3d5a6e' },
  { id: 'mcold', name: 'mCOLD', desc: 'กล่องติดตามอุณหภูมิระหว่างขนส่งยาและเวชภัณฑ์', color: 'var(--teal)' },
  { id: 'smt', name: 'SMT', desc: 'ผลิตภัณฑ์ SMT', color: '#5a6b85' },
];

export function Products() {
  const s = useApp();
  const recording = s.devices.filter((d) => d.activeTripId).length;
  const pending = s.devices.reduce((n, d) => n + pendingTrips(s, d).length, 0);
  const open = (p) => {
    if (p.id === 'mcold') nav.push({ name: 'mcold' });
    else nav.push({ name: 'placeholder', product: p });
  };
  return (
    <div className="screen">
      <AppBar back={false} title={APP_NAME} sub="แอปเดียวสำหรับอุปกรณ์ cold chain ทุกรุ่น" />
      <div className="content stack">
        <div className="hero">
          <div className="eyebrow">เลือกผลิตภัณฑ์</div>
          <h2>วันนี้ทำงานกับอุปกรณ์รุ่นไหน</h2>
          <p>แตะผลิตภัณฑ์เพื่อดูอุปกรณ์และเริ่มงาน</p>
        </div>
        {PRODUCTS.map((p) => (
          <button key={p.id} className="product" onClick={() => open(p)}>
            <div className="logo" style={{ background: p.color }}>
              {p.name}
            </div>
            <div className="stack tight" style={{ minWidth: 0 }}>
              <div className="name">{p.name}</div>
              <div className="desc">{p.desc}</div>
              {p.id === 'mcold' && (
                <div className="row wrap" style={{ gap: 6 }}>
                  <Chip tone="outline">{s.devices.length} เครื่อง</Chip>
                  {recording > 0 && <Chip tone="info" dot>กำลังบันทึก {recording}</Chip>}
                  {pending > 0 && <Chip tone="warn">รอส่ง {pending} ทริป</Chip>}
                </div>
              )}
              {p.id !== 'mcold' && <div className="tiny muted">อยู่ระหว่างย้ายเข้าแอปนี้</div>}
            </div>
            <Icon name="chevron-right" className="muted" />
          </button>
        ))}
        <div className="row small muted" style={{ justifyContent: 'center', marginTop: 4 }}>
          <Icon name={s.internet ? 'world' : 'world-off'} size={16} />
          {s.internet ? 'มือถือเชื่อมต่ออินเทอร์เน็ต' : 'มือถือไม่มีอินเทอร์เน็ต'}
        </div>
      </div>
    </div>
  );
}

export function Placeholder({ product }) {
  return (
    <div className="screen">
      <AppBar title={product.name} backLabel="ผลิตภัณฑ์" />
      <div className="content stack" style={{ justifyContent: 'center', textAlign: 'center', alignItems: 'center' }}>
        <div className="dev-icon off" style={{ width: 72, height: 72, borderRadius: 20 }}>
          <Icon name="package" size={34} />
        </div>
        <h2 style={{ margin: 0, color: 'var(--navy)' }}>{product.name}</h2>
        <p className="muted" style={{ margin: 0, maxWidth: 280 }}>
          หน้านี้เป็นตำแหน่งสำรองในต้นแบบ ฟังก์ชันของ {product.name} จะย้ายมาจากแอปเดิมในขั้นถัดไป
        </p>
        <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap', justifyContent: 'center' }}>
          <Btn kind="secondary" icon="chevron-left" onClick={nav.back}>
            กลับหน้าเลือกผลิตภัณฑ์
          </Btn>
          <Btn
            kind="ghost"
            onClick={() => {
              update((s) => void (s.mcoldTab = 'devices'));
              nav.replace({ name: 'mcold' });
            }}
          >
            ไปที่ mCOLD
          </Btn>
        </div>
      </div>
    </div>
  );
}
