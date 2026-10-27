// App theme (decided 2026-10-12): three pastel themes, each with a light and a dark mode, picked per
// phone by whoever uses it. Kept in localStorage apart from the demo state, so "reset demo" keeps it.
import { useSyncExternalStore } from 'react';

export const THEMES = [
  { id: 'sky', name: 'ฟ้าพาสเทล', swatch: ['#eef5fa', '#7cbfdd', '#2c5a82'] },
  { id: 'blush', name: 'ชมพูพาสเทล', swatch: ['#fbf2f5', '#e7a3b9', '#8a3d5e'] },
  { id: 'sage', name: 'เขียวมินต์', swatch: ['#eff7f3', '#82c9ad', '#2e6651'] },
];
export const MODES = [
  { value: 'light', label: 'สว่าง' },
  { value: 'dark', label: 'มืด' },
  { value: 'system', label: 'ตามเครื่อง' },
];

const KEY = 'cch-theme-v1';
const DEFAULT = { theme: 'sky', mode: 'light' };
const listeners = new Set();

function load() {
  try {
    const v = JSON.parse(localStorage.getItem(KEY) || 'null');
    if (v && THEMES.some((t) => t.id === v.theme) && MODES.some((m) => m.value === v.mode)) return v;
  } catch {
    /* private window or blocked storage: the default */
  }
  return DEFAULT;
}

let pref = load();
const dark = typeof matchMedia === 'function' ? matchMedia('(prefers-color-scheme: dark)') : null;
let snap = null;

function resolve() {
  const mode = pref.mode === 'system' ? (dark?.matches ? 'dark' : 'light') : pref.mode;
  if (!snap || snap.theme !== pref.theme || snap.mode !== pref.mode || snap.resolved !== mode) {
    snap = { ...pref, resolved: mode };
  }
  return snap;
}
const emit = () => listeners.forEach((f) => f());
dark?.addEventListener?.('change', emit);

export function setTheme(next) {
  pref = { ...pref, ...next };
  try {
    localStorage.setItem(KEY, JSON.stringify(pref));
  } catch {
    /* not kept, still applied */
  }
  emit();
}

/** { theme, mode, resolved }: resolved is 'light' or 'dark' (mode 'system' follows the phone). */
export function useTheme() {
  return useSyncExternalStore((f) => (listeners.add(f), () => listeners.delete(f)), resolve, resolve);
}
