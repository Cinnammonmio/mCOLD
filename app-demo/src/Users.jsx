// Ward and user. A box is registered to one customer ward and shared by that ward's users, so the
// app always shows who is using it, and records who started, stopped, acknowledged and sent.
// Switching user here stands in for signing in; how users really sign in is an open question.
import { useState } from 'react';
import { getState, update, updateUi, useApp, currentUser, wardOf } from './store.js';
import { disconnect, findDevice } from './sim.js';
import { Icon, Seg, Sheet } from './ui.jsx';
import { MODES, setTheme, THEMES, useTheme } from './theme.js';

const initial = (name) => name.trim().charAt(0);

export function Avatar({ name, size = 30 }) {
  return (
    <span
      aria-hidden="true"
      style={{ width: size, height: size, borderRadius: size / 2, background: 'var(--mint)', color: 'var(--teal)', display: 'inline-grid', placeItems: 'center', fontWeight: 700, fontSize: size * 0.45, flex: 'none' }}
    >
      {initial(name)}
    </span>
  );
}

function switchUser(id) {
  const s = getState();
  const u = s.users.find((x) => x.id === id);
  const sameWard = currentUser(s).wardId === u.wardId;
  update((st) => void (st.userId = id));
  if (!sameWard) {
    // a box of the old ward is not this user's to work with
    if (s.ble && findDevice(s, s.ble.sn).wardId !== u.wardId) disconnect();
    updateUi((ui) => ({ route: ui.route.filter((x) => ['products', 'mcold', 'placeholder'].includes(x.name)) }));
  }
}

/** Compact "who is using the app" button for app bars. */
export function UserChip({ compact }) {
  const s = useApp();
  const [open, setOpen] = useState(false);
  const u = currentUser(s);
  return (
    <>
      <button className="row" onClick={() => setOpen(true)} aria-label={`ผู้ใช้งาน ${u.name}`} style={{ border: 0, background: 'none', padding: '4px 6px', gap: 6, borderRadius: 10, minHeight: 44 }}>
        <Avatar name={u.name} />
        {!compact && <span className="small bold" style={{ maxWidth: 110, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{u.name.split(' ')[0]}</span>}
      </button>
      {open && <UserSheet onClose={() => setOpen(false)} />}
    </>
  );
}

/** One-line "ward · user" bar shown at the top of mCOLD. */
export function WardBar() {
  const s = useApp();
  const [open, setOpen] = useState(false);
  const u = currentUser(s);
  return (
    <>
      <button className="row" onClick={() => setOpen(true)} style={{ padding: '2px 2px', width: '100%', textAlign: 'left', border: 0, background: 'none', minHeight: 40 }}>
        <Avatar name={u.name} size={28} />
        <span className="grow small" style={{ whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
          <b>{wardOf(s, u.wardId).name}</b> <span className="muted">· {u.name}</span>
        </span>
        <span className="small" style={{ color: 'var(--teal)', fontWeight: 600 }}>เปลี่ยน</span>
      </button>
      {open && <UserSheet onClose={() => setOpen(false)} />}
    </>
  );
}

export function UserSheet({ onClose }) {
  const s = useApp();
  const u = currentUser(s);
  return (
    <Sheet onClose={onClose} label="ผู้ใช้งาน">
      <div className="stack">
        <div className="row">
          <Avatar name={u.name} size={48} />
          <div className="grow">
            <h2>{u.name}</h2>
            <div className="small muted">{u.role} · {wardOf(s, u.wardId).name}</div>
            <div className="tiny muted">{s.org}</div>
          </div>
        </div>
        <div className="small">อุปกรณ์ mCOLD ลงทะเบียนกับวอร์ด ผู้ใช้ทุกคนในวอร์ดใช้เครื่องร่วมกันได้ และแอปบันทึกว่าใครเป็นผู้เริ่ม จบ รับทราบการแจ้งเตือน และส่งข้อมูล</div>
        <ThemePicker />
        <div className="stack tight" style={{ border: '1px dashed var(--demo-line)', borderRadius: 12, padding: 10, background: 'var(--demo-bg)' }}>
          <div className="tiny bold" style={{ color: 'var(--demo)' }}>
            <Icon name="flask-2" size={13} /> โหมดจำลอง · สลับผู้ใช้งาน (แทนการเข้าสู่ระบบ)
          </div>
          {s.wards.map((w) => (
            <div key={w.id} className="stack tight">
              <div className="tiny muted bold">{w.name}</div>
              <div className="list">
                {s.users.filter((x) => x.wardId === w.id).map((x) => (
                  <button key={x.id} className="li" onClick={() => { switchUser(x.id); onClose(); }} aria-pressed={x.id === u.id}>
                    <Avatar name={x.name} />
                    <span className="grow">
                      <span className="t">{x.name}</span>
                      <span className="s">{x.role}</span>
                    </span>
                    {x.id === u.id && <Icon name="check" className="muted" />}
                  </button>
                ))}
              </div>
            </div>
          ))}
        </div>
      </div>
    </Sheet>
  );
}

/** Theme and light/dark, kept on this phone (theme.js). */
function ThemePicker() {
  const t = useTheme();
  return (
    <div className="stack tight">
      <div className="small bold"><Icon name="palette" size={15} /> ธีมของแอป</div>
      <div className="grid3" role="radiogroup" aria-label="ธีมของแอป">
        {THEMES.map((x) => (
          <button key={x.id} role="radio" aria-checked={t.theme === x.id} className={`theme-card ${t.theme === x.id ? 'on' : ''}`} onClick={() => setTheme({ theme: x.id })}>
            <span className="sw" aria-hidden="true">
              {x.swatch.map((c) => <span key={c} style={{ background: c }} />)}
            </span>
            <span className="tiny bold">{x.name}</span>
          </button>
        ))}
      </div>
      <Seg label="สว่างหรือมืด" value={t.mode} onChange={(v) => setTheme({ mode: v })} options={MODES} />
    </div>
  );
}
