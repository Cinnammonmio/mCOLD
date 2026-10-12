import { DEMO_VERSION, nav } from '../store.js';
import { AppBar, Btn, Icon } from '../ui.jsx';
import { UserChip } from '../Users.jsx';

export const APP_NAME = 'Cold Chain Hub';

const PRODUCTS = [
  { id: 'etemp', name: 'eTEMP', color: 'var(--slate)', icon: 'temperature' },
  { id: 'mcold', name: 'mCOLD', color: 'var(--teal)', icon: 'snowflake' },
  { id: 'smt', name: 'SMT', color: 'var(--slate2)', icon: 'cpu' },
];

export function Products() {
  const open = (p) => {
    if (p.id === 'mcold') nav.push({ name: 'mcold' });
    else nav.push({ name: 'placeholder', product: p });
  };
  return (
    <div className="screen">
      <AppBar back={false} title={APP_NAME} right={<UserChip />} />
      <div className="content stack" style={{ gap: 14, paddingTop: 20 }}>
        {PRODUCTS.map((p) => (
          <button key={p.id} className="product" onClick={() => open(p)} aria-label={p.name}>
            <div className="logo" style={{ background: p.color }}>
              <Icon name={p.icon} size={28} />
            </div>
            <div className="name">{p.name}</div>
            <Icon name="chevron-right" className="muted" />
          </button>
        ))}
        <div className="tiny" style={{ marginTop: 'auto', textAlign: 'center', color: 'var(--demo)' }}>{DEMO_VERSION}</div>
      </div>
    </div>
  );
}

export function Placeholder({ product }) {
  return (
    <div className="screen">
      <AppBar title={product.name} backLabel="กลับ" />
      <div className="content stack" style={{ justifyContent: 'center', textAlign: 'center', alignItems: 'center' }}>
        <div className="dev-icon off" style={{ width: 72, height: 72, borderRadius: 20 }}>
          <Icon name={product.icon || 'package'} size={34} />
        </div>
        <h2 style={{ margin: 0, color: 'var(--navy)' }}>{product.name}</h2>
        <p className="muted" style={{ margin: 0 }}>อยู่ระหว่างพัฒนา</p>
        <Btn kind="secondary" icon="chevron-left" onClick={nav.back}>
          กลับ
        </Btn>
      </div>
    </div>
  );
}
