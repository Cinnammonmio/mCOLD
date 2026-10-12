import { useEffect, useState } from 'react';
import { ICONS } from './icons.js';
import { nav, useApp } from './store.js';

export function Icon({ name, size = 20, stroke = 1.8, className = '', style }) {
  return (
    <svg
      className={`icon ${className}`}
      style={style}
      width={size}
      height={size}
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth={stroke}
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
      dangerouslySetInnerHTML={{ __html: ICONS[name] || '' }}
    />
  );
}

export function Btn({ kind = '', size = '', block, icon, busy, children, className = '', ...rest }) {
  return (
    <button
      type="button"
      className={`btn ${kind} ${size} ${block ? 'block' : ''} ${className}`}
      disabled={busy || rest.disabled}
      aria-busy={busy || undefined}
      {...rest}
    >
      {busy ? <Icon name="loader-2" className="spin" /> : icon ? <Icon name={icon} /> : null}
      {children}
    </button>
  );
}

export function Chip({ tone = '', icon, dot, children, title }) {
  return (
    <span className={`chip ${tone}`} title={title}>
      {dot && <span className="ldot" />}
      {icon && <Icon name={icon} size={14} stroke={2} />}
      {children}
    </span>
  );
}

export function Banner({ tone = 'info', icon, title, children, actions }) {
  const ic = icon || { ok: 'circle-check', warn: 'alert-triangle', bad: 'alert-circle', info: 'info-circle', neutral: 'info-circle', demo: 'flask-2' }[tone];
  return (
    <div className={`banner ${tone}`} role={tone === 'bad' ? 'alert' : undefined}>
      <Icon name={ic} />
      <div className="grow">
        {title && <b>{title}</b>}
        {title && children ? <div>{children}</div> : children}
        {actions && <div className="actions">{actions}</div>}
      </div>
    </div>
  );
}

export function AppBar({ title, sub, back = true, backLabel = 'กลับ', onBack, right }) {
  return (
    <header className="appbar">
      {back && (
        <button className="backbtn" onClick={onBack || nav.back} aria-label={backLabel}>
          <Icon name="chevron-left" size={22} />
          {backLabel}
        </button>
      )}
      <div className="grow" style={{ paddingLeft: back ? 0 : 6 }}>
        <h1>{title}</h1>
        {sub && <div className="sub">{sub}</div>}
      </div>
      {right}
    </header>
  );
}

export function Seg({ value, options, onChange, demo, label }) {
  return (
    <div className={`seg ${demo ? 'demo' : ''}`} role="radiogroup" aria-label={label}>
      {options.map((o) => (
        <button key={o.value} role="radio" aria-checked={value === o.value} className={value === o.value ? 'on' : ''} onClick={() => onChange(o.value)}>
          {o.label}
        </button>
      ))}
    </div>
  );
}

export function Switch({ id, checked, onChange, disabled, label }) {
  return (
    <span className="switch">
      <input id={id} type="checkbox" role="switch" aria-label={label} checked={!!checked} disabled={disabled} onChange={(e) => onChange(e.target.checked)} />
      <span />
    </span>
  );
}

export function Sheet({ onClose, children, label }) {
  useEffect(() => {
    const k = (e) => e.key === 'Escape' && onClose?.();
    window.addEventListener('keydown', k);
    return () => window.removeEventListener('keydown', k);
  }, [onClose]);
  return (
    <div className="scrim" onClick={(e) => e.target === e.currentTarget && onClose?.()}>
      <div className="sheet" role="dialog" aria-modal="true" aria-label={label}>
        <div className="grab" />
        {children}
      </div>
    </div>
  );
}

export function Toasts() {
  const { ui } = useApp();
  if (!ui.toasts.length) return null;
  return (
    <div className="toasts" aria-live="polite">
      {ui.toasts.map((t) => (
        <div key={t.id} className={`toast ${t.tone}`}>
          <Icon name={t.tone === 'ok' ? 'circle-check' : t.tone === 'bad' ? 'alert-circle' : t.tone === 'warn' ? 'alert-triangle' : 'info-circle'} />
          <span>{t.text}</span>
        </div>
      ))}
    </div>
  );
}

/** Re-render every `ms` so durations and "x minutes ago" stay current. */
export function useTick(ms = 15000) {
  const [, set] = useState(0);
  useEffect(() => {
    const t = setInterval(() => set((n) => n + 1), ms);
    return () => clearInterval(t);
  }, [ms]);
}

export function Steps({ items }) {
  return (
    <div className="steps">
      {items.map((it, i) => (
        <div key={it.key || i} className={`step ${it.state}`}>
          <div className="b">
            {it.state === 'done' ? (
              <Icon name="check" size={15} stroke={3} />
            ) : it.state === 'active' ? (
              <Icon name="loader-2" size={15} stroke={2.5} className="spin" />
            ) : it.state === 'error' ? (
              <Icon name="x" size={15} stroke={3} />
            ) : it.state === 'wait' ? (
              <Icon name="hourglass-high" size={14} stroke={2.4} />
            ) : (
              i + 1
            )}
          </div>
          <div>
            <div className="t">{it.title}</div>
            {it.sub && <div className="s">{it.sub}</div>}
            {it.extra}
          </div>
        </div>
      ))}
    </div>
  );
}

export function Field({ label, htmlFor, help, error, children }) {
  return (
    <div className="field">
      {label && <label htmlFor={htmlFor}>{label}</label>}
      {children}
      {error ? <div className="err">{error}</div> : help ? <div className="help">{help}</div> : null}
    </div>
  );
}

export function BatteryIcon({ pct }) {
  if (pct == null) return <Icon name="battery-2" size={16} />;
  const n = pct < 15 ? 'battery-exclamation' : pct < 40 ? 'battery-1' : pct < 70 ? 'battery-2' : pct < 90 ? 'battery-3' : 'battery-4';
  return <Icon name={n} size={16} />;
}
