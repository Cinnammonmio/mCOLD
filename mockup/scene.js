// Shared pieces for the mockup pages: renderer, materials, the case parts from
// build/stl (centred on the case), the e-paper screen, LEDs, front print and the
// thermocouple. Coordinates are mm, case front = +Z, case centre = origin.
import * as THREE from 'three';
import {STLLoader} from 'three/addons/loaders/STLLoader.js';
import {RoomEnvironment} from 'three/addons/environments/RoomEnvironment.js';

export const Q = new URLSearchParams(location.search);
export const qjson = (k, d) => JSON.parse(Q.get(k) || JSON.stringify(d));

// centre of the case in STEP coordinates
export const C = new THREE.Vector3(-21.5, 5.8, 15.65);
export const FRONT_Z = 6.15;          // front face of Main Case_BOT after centring
export const WIN = {w: 48, h: 23};    // display window, centred on the origin
export const LED_Y = -24, LED_X = [-7.5, 0, 7.5];   // front light pipes, left to right
export const COL = {navy: '#0b3550', teal: '#0e8391', teal2: '#1aa5b5', muted: '#5f707c'};
// front LEDs by position (docs/led-design.md): left CARGO red, middle TRIP cyan, right DEVICE amber
export const LED_COL = [0xff3b3b, 0x22c8f0, 0xffb020];

export async function fonts() {
  for (const w of [400, 500, 600, 700]) await document.fonts.load(`${w} 40px Plex`);
  await document.fonts.ready;
}

export function renderer(canvas, W, H) {
  const R = new THREE.WebGLRenderer({canvas, antialias: true, alpha: true, preserveDrawingBuffer: true});
  R.setPixelRatio(window.devicePixelRatio); R.setSize(W, H, false);
  R.toneMapping = THREE.ACESFilmicToneMapping; R.toneMappingExposure = 1.05;
  R.outputColorSpace = THREE.SRGBColorSpace;
  R.shadowMap.enabled = true; R.shadowMap.type = THREE.VSMShadowMap;
  return R;
}

export function studio(R, S, {floorY = null, key = [-90, 260, 170], shadowOpacity = .16} = {}) {
  const pm = new THREE.PMREMGenerator(R);
  S.environment = pm.fromScene(new RoomEnvironment(), 0.04).texture;
  const k = new THREE.DirectionalLight(0xffffff, 2.0); k.position.set(...key);
  if (floorY !== null) {
    k.castShadow = true; k.shadow.mapSize.set(2048, 2048);
    Object.assign(k.shadow.camera, {left: -260, right: 260, top: 260, bottom: -260, near: 10, far: 900});
    k.shadow.radius = 26; k.shadow.blurSamples = 32; k.shadow.bias = -0.0006;
    const f = new THREE.Mesh(new THREE.PlaneGeometry(3000, 3000), new THREE.ShadowMaterial({opacity: shadowOpacity}));
    f.rotation.x = -Math.PI / 2; f.position.y = floorY; f.receiveShadow = true; S.add(f);
  }
  S.add(k);
  const rim = new THREE.DirectionalLight(0xbfefff, 1.1); rim.position.set(220, 60, -120); S.add(rim);
  S.add(new THREE.HemisphereLight(0xffffff, 0xd8e8ee, 0.55));
}

export const M = {
  front: () => new THREE.MeshPhysicalMaterial({color: 0xf3f6f7, roughness: .42, clearcoat: .35, clearcoatRoughness: .35}),
  back: () => new THREE.MeshPhysicalMaterial({color: 0x0f6f80, roughness: .38, metalness: .05, clearcoat: .5, clearcoatRoughness: .25}),
  pipe: () => new THREE.MeshPhysicalMaterial({color: 0xe9f1f3, roughness: .25, clearcoat: 1, transparent: true, opacity: .92}),
  eink: () => new THREE.MeshStandardMaterial({color: 0x2a3034, roughness: .55}),
  plate: () => new THREE.MeshPhysicalMaterial({color: 0xd9dfe3, roughness: .55, clearcoat: .2}),
  board: () => new THREE.MeshPhysicalMaterial({color: 0x123f36, roughness: .45, clearcoat: .6, clearcoatRoughness: .3}),
  metal: () => new THREE.MeshPhysicalMaterial({color: 0xc9ced2, metalness: 1, roughness: .3}),
  tin: () => new THREE.MeshPhysicalMaterial({color: 0xd8d2c0, metalness: 1, roughness: .35}),
  tan: () => new THREE.MeshStandardMaterial({color: 0xb59a6a, roughness: .6}),
  black: () => new THREE.MeshStandardMaterial({color: 0x202326, roughness: .5}),
  battery: () => new THREE.MeshPhysicalMaterial({color: 0xbfc6cc, metalness: .85, roughness: .32}),
};

const L = new STLLoader();
export const load = f => new Promise((res, rej) => L.load('build/stl/' + f + '.stl', g => {
  g.translate(-C.x, -C.y, -C.z); g.computeVertexNormals(); res(g);
}, undefined, rej));

export const PART = {
  bot: 'Main_Case_BOT_80CM_24_08_26_V2x_2', top: 'Main_Case_TOP_80CM_24_08_26_V2x_3',
  eink: '2_13_Flexible_Monochrome_eInk_212x104_v1_8', plate: 'Plate_support_LCD_9',
  nfc: '3D_PCB_eSENSEV1_P_1_1_2026_08_24_NFC_1', battery: 'Baterry_59x37x5_4',
  pipes: ['Resin_5', 'Resin_6', 'Resin_7'], side: 'Resin_10',
  pcb: ['board', 'metal', 'tin', 'tan', 'black'].map(k => 'pcb_' + k),
};

export function mesh(g, m, parent, shadow = true) {
  const o = new THREE.Mesh(g, m); o.castShadow = shadow; parent.add(o); return o;
}

function canvasTex(c) {
  const t = new THREE.CanvasTexture(c); t.colorSpace = THREE.SRGBColorSpace; t.anisotropy = 8; return t;
}

// The A2 TRIP ACTIVE screen, sitting just under the front face
export async function screen(parent, z = 5.62) {
  const tex = await new THREE.TextureLoader().loadAsync('build/screen_tex.png');
  tex.colorSpace = THREE.SRGBColorSpace; tex.anisotropy = 8;
  const s = new THREE.Mesh(new THREE.PlaneGeometry(WIN.w + .4, WIN.h + .6), new THREE.MeshPhysicalMaterial({
    map: tex, roughness: .55, clearcoat: .6, clearcoatRoughness: .15, emissive: 0xffffff, emissiveMap: tex, emissiveIntensity: .18}));
  s.position.z = z; parent.add(s); return s;
}

// Dark printed lens frame round the display window so the screen reads as a framed panel
export function bezel(parent, z = FRONT_Z + 0.03) {
  const rr = (s, w, h, r) => {
    s.moveTo(-w / 2 + r, -h / 2); s.lineTo(w / 2 - r, -h / 2); s.quadraticCurveTo(w / 2, -h / 2, w / 2, -h / 2 + r);
    s.lineTo(w / 2, h / 2 - r); s.quadraticCurveTo(w / 2, h / 2, w / 2 - r, h / 2); s.lineTo(-w / 2 + r, h / 2);
    s.quadraticCurveTo(-w / 2, h / 2, -w / 2, h / 2 - r); s.lineTo(-w / 2, -h / 2 + r); s.quadraticCurveTo(-w / 2, -h / 2, -w / 2 + r, -h / 2);
    return s;
  };
  const outer = rr(new THREE.Shape(), WIN.w + 9, WIN.h + 9, 3.2);
  outer.holes.push(rr(new THREE.Path(), WIN.w, WIN.h, 0.6));
  const g = new THREE.ExtrudeGeometry(outer, {depth: .35, bevelEnabled: true, bevelThickness: .15, bevelSize: .25, bevelSegments: 3, curveSegments: 24});
  const m = new THREE.Mesh(g, new THREE.MeshPhysicalMaterial({color: 0x18252e, roughness: .18, clearcoat: 1, clearcoatRoughness: .05}));
  m.position.z = z; parent.add(m);
  // thin teal keyline on the outer edge
  const ring = rr(new THREE.Shape(), WIN.w + 10.6, WIN.h + 10.6, 3.9);
  ring.holes.push(rr(new THREE.Path(), WIN.w + 9.4, WIN.h + 9.4, 3.3));
  const k = new THREE.Mesh(new THREE.ShapeGeometry(ring, 24), new THREE.MeshPhysicalMaterial({color: 0x1aa5b5, roughness: .3, clearcoat: .6, polygonOffset: true, polygonOffsetFactor: -2}));
  k.position.z = FRONT_Z + 0.02; parent.add(k);
  return m;
}

// Front LEDs: lit light pipes, a soft halo each and the CARGO / TRIP / DEVICE print under them
export function leds(parent, pipes, colors = LED_COL, {labels = true} = {}) {
  pipes.forEach((p, i) => {
    const c = new THREE.Color(colors[i]);
    p.material = new THREE.MeshBasicMaterial({color: c, toneMapped: false});
    const cv = document.createElement('canvas'); cv.width = cv.height = 128; const x = cv.getContext('2d');
    const g = x.createRadialGradient(64, 64, 0, 64, 64, 64), s = `${Math.round(c.r * 255)},${Math.round(c.g * 255)},${Math.round(c.b * 255)}`;
    g.addColorStop(0, `rgba(${s},.6)`); g.addColorStop(.32, `rgba(${s},.22)`); g.addColorStop(1, `rgba(${s},0)`);
    x.fillStyle = g; x.fillRect(0, 0, 128, 128);
    const h = new THREE.Mesh(new THREE.PlaneGeometry(10, 10), new THREE.MeshBasicMaterial({map: canvasTex(cv), transparent: true, depthWrite: false, toneMapped: false}));
    h.position.set(LED_X[i], LED_Y, FRONT_Z + .25); parent.add(h);
    // dark ring so an unlit pipe still reads against the white case
    const r = new THREE.Mesh(new THREE.RingGeometry(1.55, 2.3, 48), new THREE.MeshPhysicalMaterial({color: 0x9fb0b8, roughness: .4, polygonOffset: true, polygonOffsetFactor: -2}));
    r.position.set(LED_X[i], LED_Y, FRONT_Z + .02); parent.add(r);
  });
  if (!labels) return;
  const cv = document.createElement('canvas'); cv.width = 1440; cv.height = 160; const x = cv.getContext('2d');
  x.font = '700 80px Plex'; x.textAlign = 'center'; x.textBaseline = 'middle'; x.fillStyle = '#5f707c';
  ['CARGO', 'TRIP', 'DEVICE'].forEach((t, i) => x.fillText(t, 720 + LED_X[i] * 1440 / 36, 80));
  const m = new THREE.Mesh(new THREE.PlaneGeometry(36, 4), new THREE.MeshPhysicalMaterial({map: canvasTex(cv), transparent: true, roughness: .4, polygonOffset: true, polygonOffsetFactor: -2}));
  m.position.set(0, LED_Y - 5, FRONT_Z + .02); parent.add(m);
}

// mCOLD wordmark printed above the screen
export function frontPrint(parent) {
  const c = document.createElement('canvas'); c.width = 1600; c.height = 480; const x = c.getContext('2d');
  x.font = '700 170px Plex'; x.textBaseline = 'middle';
  const w1 = x.measureText('m').width, w2 = x.measureText('COLD').width, sx = 800 - (w1 + w2) / 2;
  x.fillStyle = COL.navy; x.fillText('m', sx, 190); x.fillStyle = COL.teal; x.fillText('COLD', sx + w1, 190);
  x.textAlign = 'center'; x.font = '600 54px Plex'; x.fillStyle = '#6b7f8b'; x.letterSpacing = '14px'; x.fillText('COLD-CHAIN MONITOR', 800, 370);
  const m = new THREE.Mesh(new THREE.PlaneGeometry(40, 12), new THREE.MeshPhysicalMaterial({map: canvasTex(c), transparent: true, roughness: .4, clearcoat: .35, polygonOffset: true, polygonOffsetFactor: -2}));
  m.position.set(0, 28.5, FRONT_Z + .02); parent.add(m);
}

// Whole assembled device in a group, ready to place
export async function device(parent) {
  const g = new THREE.Group(); parent.add(g);
  mesh(await load(PART.bot), M.front(), g);
  mesh(await load(PART.top), M.back(), g);
  mesh(await load(PART.eink), M.eink(), g);
  const pipes = [];
  for (const p of PART.pipes) pipes.push(mesh(await load(p), M.pipe(), g));
  mesh(await load(PART.side), M.pipe(), g);
  await screen(g); bezel(g); leds(g, pipes); frontPrint(g);
  return g;
}

// ---------- thermocouple ----------
export function braidTexture() {
  const c = document.createElement('canvas'); c.width = 512; c.height = 256; const x = c.getContext('2d');
  x.fillStyle = '#8d959b'; x.fillRect(0, 0, 512, 256);
  const n = 16, cw = 512 / n, ch = 256 / 8;
  for (let i = 0; i < n; i++) for (let j = 0; j < 8; j++) {
    const dir = (i + j) % 2, blue = j === i % 8 || j === (i + 4) % 8;
    const g = x.createLinearGradient(i * cw, j * ch, (i + 1) * cw, (j + 1) * ch);
    if (blue) { g.addColorStop(0, '#1f78c8'); g.addColorStop(.5, '#3aa0ec'); g.addColorStop(1, '#14609f'); }
    else { g.addColorStop(0, '#c9d0d5'); g.addColorStop(.45, '#f6f8f9'); g.addColorStop(1, '#9aa3a9'); }
    x.save(); x.translate(i * cw + cw / 2, j * ch + ch / 2); x.rotate(dir ? 0.5 : -0.5);
    x.fillStyle = g; x.beginPath(); x.roundRect(-cw * .62, -ch * .34, cw * 1.24, ch * .68, ch * .3); x.fill();
    x.strokeStyle = 'rgba(60,66,72,.55)'; x.lineWidth = 1.5;
    for (let k = -2; k <= 2; k++) { x.beginPath(); x.moveTo(-cw * .55, k * ch * .12); x.lineTo(cw * .55, k * ch * .12); x.stroke(); }
    x.restore();
  }
  const t = canvasTex(c); t.wrapS = t.wrapT = THREE.RepeatWrapping; return t;
}

function engraving(fromTip) {
  const c = document.createElement('canvas'); c.width = 1024; c.height = 2048; const x = c.getContext('2d');
  x.fillStyle = '#d4d8db'; x.fillRect(0, 0, 1024, 2048);
  // brushed finish runs along the sheath (fixed seed so renders repeat exactly)
  let s = 7; const rnd = () => (s = (s * 16807) % 2147483647) / 2147483647;
  for (let i = 0; i < 700; i++) { const v = rnd() < .5 ? 255 : 110; x.strokeStyle = `rgba(${v},${v},${v + 5},.05)`; x.beginPath(); const u = rnd() * 1024; x.moveTo(u, 0); x.lineTo(u + rnd() * 2, 2048); x.stroke(); }
  // laser marking (canvas top = tip end); fromTip reads tip -> collar like the real part,
  // otherwise collar -> tip, so the text can be upright whichever way the probe points
  x.save();
  if (fromTip) { x.translate(256, 250); x.rotate(Math.PI / 2); } else { x.translate(256, 2048 - 250); x.rotate(-Math.PI / 2); }
  x.font = '700 118px Plex'; x.textBaseline = 'middle'; const txt = 'T/C K(S) 75309005 /08-26';
  x.scale(Math.min(1, 1560 / x.measureText(txt).width), 1); x.fillStyle = '#2a2e32'; x.fillText(txt, 0, 0); x.restore();
  return canvasTex(c);
}

// Braided cable along curve, probe at its end lying along the end tangent (flattened to the floor)
export function thermocouple(S, curve, {floorY, cr = 1.9, spin = 4.71, fromTip = false, contact = true} = {}) {
  const bt = braidTexture(); bt.repeat.set(curve.getLength() / 5.0, 1);
  const braid = new THREE.MeshPhysicalMaterial({map: bt, metalness: .75, roughness: .38, clearcoat: .2});
  const cable = new THREE.Mesh(new THREE.TubeGeometry(curve, 900, cr, 24, false), braid); cable.castShadow = true; S.add(cable);
  const end = curve.getPoint(1), tan = curve.getTangent(1).setY(0).normalize();
  const steel = new THREE.MeshPhysicalMaterial({color: 0xd6dadd, metalness: 1, roughness: .24, clearcoat: .3});
  const probe = new THREE.Group(); S.add(probe);
  const L = 34, sr = 2.45;
  const collar = new THREE.Mesh(new THREE.CylinderGeometry(2.75, 2.75, 5.5, 48), steel); collar.position.y = 2.75; probe.add(collar);
  const slot = new THREE.Mesh(new THREE.BoxGeometry(1.2, 2.2, .6), new THREE.MeshStandardMaterial({color: 0x444a50, metalness: .6, roughness: .5})); slot.position.set(0, 3.2, 2.6); probe.add(slot);
  const neck = new THREE.Mesh(new THREE.CylinderGeometry(sr, 2.75, .8, 48), steel); neck.position.y = 5.9; probe.add(neck);
  const sh = new THREE.Mesh(new THREE.CylinderGeometry(sr, sr, L, 64, 1, true), new THREE.MeshPhysicalMaterial({map: engraving(fromTip), metalness: 1, roughness: .26, clearcoat: .3}));
  sh.position.y = 6.3 + L / 2; probe.add(sh);
  const tip = new THREE.Mesh(new THREE.CylinderGeometry(.35, sr, 3.2, 64), steel); tip.position.y = 6.3 + L + 1.6; probe.add(tip);
  probe.traverse(o => { if (o.isMesh) o.castShadow = true; });
  probe.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), tan); probe.rotateY(spin);
  probe.position.copy(end).setY(floorY + 2.75);
  const f = new THREE.Mesh(new THREE.CylinderGeometry(cr, cr, 3, 24), braid);
  f.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), tan); f.position.copy(end).addScaledVector(tan, -.5).setY(floorY + cr); S.add(f);
  if (contact) {
    // contact shadows for the thin parts (VSM blur washes them out)
    const sm = new THREE.MeshBasicMaterial({color: 0x0b3550, transparent: true, opacity: .10, depthWrite: false});
    const pts = curve.getPoints(400).filter(v => v.y < floorY + cr + 3).map(v => new THREE.Vector3(v.x + 1.2, floorY + .05, v.z - .8));
    if (pts.length > 4) S.add(new THREE.Mesh(new THREE.TubeGeometry(new THREE.CatmullRomCurve3(pts), 600, cr * 1.25, 8, false), sm));
    const ps = new THREE.Mesh(new THREE.CylinderGeometry(3, 3, 44, 24), sm); ps.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), tan);
    ps.scale.set(1, 1, .15); ps.position.copy(end).addScaledVector(tan, 22).setY(floorY + .05); S.add(ps);
  }
  return {probe, end, tan};
}

// ---------- 2D overlay helpers ----------
export const projector = (cam, W, H) => v => { const p = v.clone().project(cam); return [(p.x + 1) / 2 * W, (1 - p.y) / 2 * H]; };

// leader line from anchor [ax,ay] to a label at (tx,ty); side 'r' = label right of the elbow, 'l' = left
export function callout(svg, box, [ax, ay], tx, ty, side, title, sub, {w = 320} = {}) {
  const ex = side === 'r' ? tx - 14 : tx + 14, kx = side === 'r' ? ex - 24 : ex + 24;
  svg.insertAdjacentHTML('beforeend', `<polyline points="${ax},${ay} ${kx},${ty + 12} ${ex},${ty + 12}" fill="none" stroke="${COL.teal}" stroke-width="1.6" stroke-opacity=".75"/><circle cx="${ax}" cy="${ay}" r="5.5" fill="#fff" stroke="${COL.teal}" stroke-width="2.4"/>`);
  const d = document.createElement('div'); d.className = 'co ' + side; d.innerHTML = `<b>${title}</b>${sub ? `<small>${sub}</small>` : ''}`;
  d.style.top = ty + 'px'; d.style.width = w + 'px'; d.style.left = (side === 'r' ? tx : tx - w) + 'px';
  box.appendChild(d); return d;
}
