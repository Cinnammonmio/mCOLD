// Concept screens of the TCOLD phone app, drawn in HTML for the mockup pages.
// The numbers match the A2 TRIP ACTIVE e-paper screen (TCOLD-0117, 14:32, 4.2 °C,
// MIN 2.8, MAX 6.1, battery 78 %, storage 6 %) so the phone and the box agree.

export const PHONE_CSS = `
.phone{position:absolute;width:330px;height:690px;border-radius:52px;background:#0e1418;padding:11px;
  box-shadow:0 40px 80px rgba(11,53,80,.25),0 10px 24px rgba(11,53,80,.18),inset 0 0 0 2px #2c3840,inset 0 0 0 4px #0e1418}
.phone .scr{position:relative;width:100%;height:100%;border-radius:42px;overflow:hidden;background:#f4f7f9;font-family:Plex,sans-serif;color:#14293a}
.phone .island{position:absolute;top:11px;left:50%;transform:translateX(-50%);width:96px;height:28px;border-radius:20px;background:#0e1418;z-index:3}
.phone .sb{height:50px;display:flex;justify-content:space-between;align-items:center;padding:6px 28px 0 32px;font-size:14px;font-weight:600}
.phone .sb svg{height:12px}
.phone .hdr{display:flex;align-items:center;gap:10px;padding:4px 18px 12px}
.phone .hdr .t{font-size:17px;font-weight:700;color:#0b3550;line-height:1.1}
.phone .hdr .s{font-size:11.5px;color:#5f707c;display:flex;align-items:center;gap:4px}
.phone .hdr .s i{width:7px;height:7px;border-radius:50%;background:#1d8a5a;display:inline-block}
.phone .hdr .ble{margin-left:auto;width:34px;height:34px;border-radius:12px;background:#e6f5f6;color:#0e8391;display:flex;align-items:center;justify-content:center}
.phone .hdr .ble svg{width:18px;height:18px}
.phone .hero{margin:0 14px;border-radius:22px;padding:14px 16px 14px;color:#fff;background:linear-gradient(140deg,#0b3550 0%,#0e6f80 70%,#1aa5b5 100%);position:relative;overflow:hidden}
.phone .hero:after{content:"";position:absolute;right:-40px;top:-40px;width:150px;height:150px;border-radius:50%;background:rgba(255,255,255,.07)}
.phone .pill{display:inline-flex;align-items:center;gap:6px;font-size:11px;font-weight:600;background:rgba(255,255,255,.16);padding:3px 10px;border-radius:99px}
.phone .pill i{width:7px;height:7px;border-radius:50%;background:#5ee0ff;box-shadow:0 0 0 3px rgba(94,224,255,.25)}
.phone .big{font-size:52px;font-weight:700;line-height:1;margin-top:10px;letter-spacing:-.02em}
.phone .big sup{font-size:20px;font-weight:500;vertical-align:28px;margin-left:2px}
.phone .rng{display:flex;justify-content:space-between;align-items:center;margin-top:8px;font-size:11.5px;color:#cfe3ea}
.phone .ok{background:#e2f3eb;color:#1d8a5a;font-weight:600;border-radius:99px;padding:2px 9px;font-size:11px}
.phone .card{margin:10px 14px 0;background:#fff;border-radius:18px;padding:11px 12px 8px;border:1px solid #e3eaee}
.phone .card h4{font-size:12.5px;font-weight:600;color:#0b3550;display:flex;justify-content:space-between}
.phone .card h4 span{font-weight:400;color:#8b9aa5;font-size:11px}
.phone .stats{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin:10px 14px 0}
.phone .stats div{background:#fff;border:1px solid #e3eaee;border-radius:14px;padding:7px 10px}
.phone .stats small{display:block;font-size:10.5px;color:#5f707c}
.phone .stats b{font-size:18px;font-weight:700;color:#0b3550}
.phone .stats b.z{color:#1d8a5a}
.phone .dev{display:flex;justify-content:space-between;margin:10px 14px 0;padding:8px 12px;background:#fff;border:1px solid #e3eaee;border-radius:14px;font-size:11.5px;color:#33495a}
.phone .dev span{display:flex;align-items:center;gap:5px}
.phone .dev svg{width:15px;height:15px;color:#0e8391}
.phone .btns{display:grid;grid-template-columns:1fr 1fr;gap:9px;margin:12px 14px 0}
.phone .btns div{height:42px;border-radius:14px;display:flex;align-items:center;justify-content:center;gap:6px;font-size:13px;font-weight:600}
.phone .btns svg{width:16px;height:16px}
.phone .b1{border:1.5px solid #0e8391;color:#0e8391;background:#fff}
.phone .b2{background:#0b3550;color:#fff}
.phone .home{position:absolute;bottom:8px;left:50%;transform:translateX(-50%);width:120px;height:5px;border-radius:3px;background:#14293a;opacity:.85}
`;

// temperature every 5 min from 08:07 to 14:32 (78 samples), min 2.8, max 6.1, last 4.2
export function tripSeries() {
  const keys = [[0, 5.4], [6, 3.9], [14, 3.4], [22, 3.9], [28, 3.1], [33, 2.8], [40, 3.3], [47, 4.0], [52, 6.1], [56, 5.2], [62, 4.4], [69, 3.9], [77, 4.2]];
  const out = [];
  for (let i = 0; i <= 77; i++) {
    let k = 0; while (keys[k + 1][0] < i) k++;
    const [x0, y0] = keys[k], [x1, y1] = keys[k + 1], t = (i - x0) / (x1 - x0), e = (1 - Math.cos(Math.PI * t)) / 2;
    const atKey = keys.some(([x]) => x === i);
    out.push(atKey ? (keys.find(([x]) => x === i)[1]) : Math.min(6.05, Math.max(2.85, y0 + (y1 - y0) * e + 0.07 * Math.sin(i * 1.7))));
  }
  return out;
}

function chart(w = 274, h = 118) {
  const s = tripSeries(), pad = {l: 20, r: 6, t: 8, b: 16}, X = i => pad.l + i / 77 * (w - pad.l - pad.r);
  const Y = v => pad.t + (10 - v) / 10 * (h - pad.t - pad.b);   // axis 0..10 °C
  const line = s.map((v, i) => `${i ? 'L' : 'M'}${X(i).toFixed(1)},${Y(v).toFixed(1)}`).join('');
  const area = line + `L${X(77)},${Y(0)}L${X(0)},${Y(0)}Z`;
  const ticks = [['08:00', -1.4], ['10:00', 22.6], ['12:00', 46.6], ['14:00', 70.6]];
  return `<svg width="${w}" height="${h}" viewBox="0 0 ${w} ${h}" style="display:block;margin-top:4px">
    <defs><linearGradient id="ga" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#1aa5b5" stop-opacity=".28"/><stop offset="1" stop-color="#1aa5b5" stop-opacity="0"/></linearGradient></defs>
    <rect x="${pad.l}" y="${Y(8)}" width="${w - pad.l - pad.r}" height="${Y(2) - Y(8)}" fill="#e2f3eb"/>
    <line x1="${pad.l}" x2="${w - pad.r}" y1="${Y(8)}" y2="${Y(8)}" stroke="#8fd0b0" stroke-dasharray="3 3"/>
    <line x1="${pad.l}" x2="${w - pad.r}" y1="${Y(2)}" y2="${Y(2)}" stroke="#8fd0b0" stroke-dasharray="3 3"/>
    <text x="2" y="${Y(8) + 4}" font-size="10" fill="#5f707c">8°</text><text x="2" y="${Y(2) + 4}" font-size="10" fill="#5f707c">2°</text>
    <path d="${area}" fill="url(#ga)"/><path d="${line}" fill="none" stroke="#0e8391" stroke-width="2.2" stroke-linejoin="round"/>
    <circle cx="${X(77)}" cy="${Y(4.2)}" r="4.5" fill="#fff" stroke="#0e8391" stroke-width="2.5"/>
    ${ticks.map(([t, i]) => `<text x="${X(Math.max(0, i))}" y="${h - 2}" font-size="9.5" fill="#8b9aa5" text-anchor="${i < 0 ? 'start' : 'middle'}">${t}</text>`).join('')}
  </svg>`;
}

const ic = {
  ble: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M7 8l10 8l-5 4l0 -16l5 4l-10 8"/></svg>',
  bat: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M6 7h11a2 2 0 0 1 2 2v.5a.5 .5 0 0 0 .5 .5a.5 .5 0 0 1 .5 .5v3a.5 .5 0 0 1 -.5 .5a.5 .5 0 0 0 -.5 .5v.5a2 2 0 0 1 -2 2h-11a2 2 0 0 1 -2 -2v-6a2 2 0 0 1 2 -2"/><path d="M7 10v4M10 10v4M13 10v4"/></svg>',
  db: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 6m-8 0a8 3 0 1 0 16 0a8 3 0 1 0 -16 0"/><path d="M4 6v6a8 3 0 0 0 16 0v-6"/><path d="M4 12v6a8 3 0 0 0 16 0v-6"/></svg>',
  pin: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M9 11a3 3 0 1 0 6 0a3 3 0 0 0 -6 0"/><path d="M17.657 16.657l-4.243 4.243a2 2 0 0 1 -2.827 0l-4.244 -4.243a8 8 0 1 1 11.314 0z"/></svg>',
  wifi: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 18l.01 0"/><path d="M9.172 15.172a4 4 0 0 1 5.656 0"/><path d="M6.343 12.343a8 8 0 0 1 11.314 0"/><path d="M3.515 9.515c4.686 -4.687 12.284 -4.687 17 0"/></svg>',
  csv: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M14 3v4a1 1 0 0 0 1 1h4"/><path d="M17 21h-10a2 2 0 0 1 -2 -2v-14a2 2 0 0 1 2 -2h7l5 5v11a2 2 0 0 1 -2 2z"/><path d="M12 11v6M9.5 14.5l2.5 2.5l2.5 -2.5"/></svg>',
  stop: '<svg viewBox="0 0 24 24" fill="currentColor"><rect x="6" y="6" width="12" height="12" rx="2"/></svg>',
};

export function tripScreen() {
  return `<div class="scr"><div class="island"></div>
  <div class="sb"><span>14:32</span><span style="display:flex;gap:6px;align-items:center">
    <svg viewBox="0 0 18 12"><rect x="0" y="8" width="3" height="4" rx="1" fill="#14293a"/><rect x="5" y="5" width="3" height="7" rx="1" fill="#14293a"/><rect x="10" y="2" width="3" height="10" rx="1" fill="#14293a"/><rect x="15" y="0" width="3" height="12" rx="1" fill="#14293a"/></svg>
    <svg viewBox="0 0 26 12"><rect x=".5" y=".5" width="22" height="11" rx="3" fill="none" stroke="#14293a" opacity=".5"/><rect x="2" y="2" width="16" height="8" rx="2" fill="#14293a"/><rect x="23.5" y="4" width="2" height="4" rx="1" fill="#14293a" opacity=".5"/></svg></span></div>
  <div class="hdr"><div><div class="t">TCOLD-0117</div><div class="s"><i></i>เชื่อมต่อแล้ว ผ่าน Bluetooth</div></div><div class="ble">${ic.ble}</div></div>
  <div class="hero"><span class="pill"><i></i>ทริปกำลังทำงาน</span>
    <div class="big">4.2<sup>°C</sup></div>
    <div class="rng"><span>เริ่ม 08:07 · 6 ชม. 25 น.</span><span class="ok">ในช่วง 2–8 °C</span></div></div>
  <div class="card"><h4>อุณหภูมิตลอดทริป <span>บันทึกทุก 5 นาที</span></h4>${chart()}</div>
  <div class="stats"><div><small>ต่ำสุด</small><b>2.8°</b></div><div><small>สูงสุด</small><b>6.1°</b></div><div><small>Alarm</small><b class="z">0</b></div></div>
  <div class="dev"><span>${ic.bat}78%</span><span>${ic.db}6%</span><span>${ic.pin}GNSS</span><span>${ic.wifi}Wi-Fi</span></div>
  <div class="btns"><div class="b1">${ic.csv}ส่งออก CSV</div><div class="b2">${ic.stop}หยุดทริป</div></div>
  <div class="home"></div></div>`;
}
