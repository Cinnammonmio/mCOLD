// Shared pieces for the mockup pages: renderer, materials, the case parts from
// build/stl (centred on the case), the e-paper screen, LEDs, front print and the
// thermocouple. Coordinates are mm, case front = +Z, case centre = origin.
import * as THREE from 'three';
import {STLLoader} from 'three/addons/loaders/STLLoader.js';
import {RoomEnvironment} from 'three/addons/environments/RoomEnvironment.js';
import {toCreasedNormals} from 'three/addons/utils/BufferGeometryUtils.js';
import {RoundedBoxGeometry} from 'three/addons/geometries/RoundedBoxGeometry.js';

export const Q = new URLSearchParams(location.search);
export const qjson = (k, d) => JSON.parse(Q.get(k) || JSON.stringify(d));

// centre of the case in STEP coordinates
export const C = new THREE.Vector3(-21.5, 5.8, 15.65);
export const FRONT_Z = 6.2;           // just above the front face of Main Case_BOT (6.12–6.197, not flat)
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
// geometries are cached, so six devices in the dock share one set
const GEO = {};
export const load = f => GEO[f] ??= new Promise((res, rej) => L.load('build/stl/' + f + '.stl', g => {
  g.translate(-C.x, -C.y, -C.z); g.computeVertexNormals(); res(g);
}, undefined, rej));
// dock parts stay in the dock STEP's own coordinates (slots open to +Y, front of the dock is -Z)
// smooth normals across facets but keep edges sharper than 35 degrees: the dock's large rounded
// surfaces otherwise show every triangle
export const loadDock = f => GEO['dock/' + f] ??= new Promise((res, rej) => L.load('build/stl_dock/' + f + '.stl', g => {
  res(toCreasedNormals(g, THREE.MathUtils.degToRad(35)));
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
export async function screen(parent, z = 5.62, file = 'screen_tex') {
  const tex = await new THREE.TextureLoader().loadAsync(`build/${file}.png`);
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
  const outer = rr(new THREE.Shape(), WIN.w + 3.2, WIN.h + 3.2, 1.4);   // 1.6 mm frame each side
  outer.holes.push(rr(new THREE.Path(), WIN.w, WIN.h, 0.6));
  const g = new THREE.ExtrudeGeometry(outer, {depth: .1, bevelEnabled: true, bevelThickness: .05, bevelSize: .15, bevelSegments: 2, curveSegments: 24});
  const m = new THREE.Mesh(g, new THREE.MeshPhysicalMaterial({color: 0x18252e, roughness: .18, clearcoat: 1, clearcoatRoughness: .05}));
  m.position.z = z; parent.add(m);
  // thin teal keyline on the outer edge
  const ring = rr(new THREE.Shape(), WIN.w + 4.4, WIN.h + 4.4, 2.0);
  ring.holes.push(rr(new THREE.Path(), WIN.w + 3.8, WIN.h + 3.8, 1.7));
  const k = new THREE.Mesh(new THREE.ShapeGeometry(ring, 24), new THREE.MeshPhysicalMaterial({color: 0x1aa5b5, roughness: .3, clearcoat: .6, polygonOffset: true, polygonOffsetFactor: -2}));
  k.position.z = FRONT_Z + 0.02; parent.add(k);
  return m;
}

// Front LEDs: lit light pipes in their position colours, no glow and no print
export function leds(parent, pipes, colors = LED_COL) {
  pipes.forEach((p, i) => {
    // null = unlit: a frosted pipe instead of a lit one
    p.material = colors[i] == null ? M.pipe() : new THREE.MeshBasicMaterial({color: new THREE.Color(colors[i]), toneMapped: false});
    // grey ring so the pipe reads against the white case
    const r = new THREE.Mesh(new THREE.RingGeometry(1.55, 2.1, 48), new THREE.MeshPhysicalMaterial({color: 0x9fb0b8, roughness: .4, polygonOffset: true, polygonOffsetFactor: -2}));
    r.position.set(LED_X[i], LED_Y, FRONT_Z + .02); parent.add(r);
  });
}

// TCOLD wordmark printed above the screen (brand/TCOLD_logo.svg) with the THANES line under it
export async function frontPrint(parent) {
  const img = new Image(); img.src = 'brand/TCOLD_logo.svg'; await img.decode();
  const c = document.createElement('canvas'); c.width = 1600; c.height = 480; const x = c.getContext('2d');
  const lw = 1180, lh = lw * img.height / img.width;
  x.drawImage(img, 800 - lw / 2, 205 - lh / 2, lw, lh);
  x.textAlign = 'center'; x.textBaseline = 'middle'; x.font = '600 54px Plex'; x.fillStyle = '#6b7f8b'; x.letterSpacing = '14px';
  x.fillText('THANES · SMART COLD CHAIN', 800, 410);
  const m = new THREE.Mesh(new THREE.PlaneGeometry(40, 12), new THREE.MeshPhysicalMaterial({map: canvasTex(c), transparent: true, roughness: .4, clearcoat: .35, polygonOffset: true, polygonOffsetFactor: -2}));
  m.position.set(0, 28.5, FRONT_Z + .02); parent.add(m);
}

// Whole assembled device in a group, ready to place
// screen: build/<screen>.png texture; lit: colour per front LED (null = off), default all three lit
export async function device(parent, {screen: scr = 'screen_tex', lit = LED_COL, side = null} = {}) {
  const g = new THREE.Group(); parent.add(g);
  mesh(await load(PART.bot), M.front(), g);
  mesh(await load(PART.top), M.back(), g);
  mesh(await load(PART.eink), M.eink(), g);
  const pipes = [];
  for (const p of PART.pipes) pipes.push(mesh(await load(p), M.pipe(), g));
  // side light: null = off, otherwise lit in that colour (green = charging)
  mesh(await load(PART.side), side == null ? M.pipe() : new THREE.MeshBasicMaterial({color: new THREE.Color(side), toneMapped: false}), g);
  await screen(g, 5.62, scr); bezel(g); leds(g, pipes, lit); await frontPrint(g);
  return g;
}

// ---------- iPhone ----------
// iPhone 15 size: 71.6 x 147.6 x 7.8 mm. Local axes: +Y = top (camera end), +Z = screen side.
export const PHONE = {w: 71.6, h: 147.6, t: 7.8, nfc: [0, 52, -3.9]};   // NFC coil sits behind the top of the back

function roundedRect(w, h, r, seg = 12) {
  const s = new THREE.Shape(), x = -w / 2, y = -h / 2;
  s.moveTo(x + r, y); s.lineTo(x + w - r, y); s.absarc(x + w - r, y + r, r, -Math.PI / 2, 0);
  s.lineTo(x + w, y + h - r); s.absarc(x + w - r, y + h - r, r, 0, Math.PI / 2);
  s.lineTo(x + r, y + h); s.absarc(x + r, y + h - r, r, Math.PI / 2, Math.PI); s.lineTo(x, y + r); s.absarc(x + r, y + r, r, Math.PI, 1.5 * Math.PI);
  const g = new THREE.ShapeGeometry(s, seg), uv = g.attributes.uv, p = g.attributes.position;
  for (let i = 0; i < uv.count; i++) uv.setXY(i, (p.getX(i) - x) / w, (p.getY(i) - y) / h);   // 0..1 over the rectangle
  return g;
}

export async function iphone(parent, screenFile = 'phone_nfc') {
  const g = new THREE.Group(); parent.add(g);
  const frame = new THREE.MeshPhysicalMaterial({color: 0x9aa3ab, metalness: 1, roughness: .28, clearcoat: .4});
  const body = new THREE.Mesh(new RoundedBoxGeometry(PHONE.w, PHONE.h, PHONE.t, 6, 3.6), frame); body.castShadow = true; g.add(body);
  // back glass, frosted, with the camera block
  const backMat = new THREE.MeshPhysicalMaterial({color: 0x5f6b75, roughness: .55, clearcoat: .6});
  const back = new THREE.Mesh(roundedRect(PHONE.w - 1.6, PHONE.h - 1.6, 9.5), backMat); back.rotation.y = Math.PI; back.position.z = -PHONE.t / 2 - .01; g.add(back);
  const bump = new THREE.Mesh(new RoundedBoxGeometry(31, 31, 1.6, 4, 5), backMat); bump.position.set(PHONE.w / 2 - 20, PHONE.h / 2 - 20, -PHONE.t / 2 - .7); g.add(bump);
  // front: black glass with the screen inside it
  const glass = new THREE.Mesh(roundedRect(PHONE.w - 1.2, PHONE.h - 1.2, 10), new THREE.MeshPhysicalMaterial({color: 0x050608, roughness: .1, clearcoat: 1}));
  glass.position.z = PHONE.t / 2 + .01; g.add(glass);
  const tex = await new THREE.TextureLoader().loadAsync(`build/${screenFile}.png`);
  tex.colorSpace = THREE.SRGBColorSpace; tex.anisotropy = 8;
  const sw = PHONE.w - 5.4, sh = sw * 2556 / 1179;
  const scr = new THREE.Mesh(roundedRect(sw, sh, 8.2), new THREE.MeshBasicMaterial({map: tex, toneMapped: false}));
  scr.position.z = PHONE.t / 2 + .03; g.add(scr);
  // a faint reflective layer so the screen reads as glass
  const sheen = new THREE.Mesh(roundedRect(sw, sh, 8.2), new THREE.MeshPhysicalMaterial({color: 0x000000, transparent: true, opacity: .04, roughness: .04, clearcoat: 1, metalness: .2}));
  sheen.position.z = PHONE.t / 2 + .05; g.add(sheen);
  // side buttons
  const btn = (x, y, h) => { const b = new THREE.Mesh(new RoundedBoxGeometry(1.2, h, 3.2, 2, .5), frame); b.position.set(x, y, 0); g.add(b); };
  btn(-PHONE.w / 2 - .4, 44, 7); btn(-PHONE.w / 2 - .4, 30, 12); btn(-PHONE.w / 2 - .4, 15, 12); btn(PHONE.w / 2 + .4, 28, 18);
  return g;
}

// ---------- 6-slot dock ----------
// Slot geometry measured from Dock Assy.STEP: slots 14 mm wide on a 20 mm pitch along X,
// floor at Y -6.3, 89 mm long along Z; the USB-C plug of each slot stands at Z 58–67.5.
export const DOCK = {
  slotX: [-70.6, -50.6, -30.6, -10.6, 9.4, 29.4], floorY: -6.3, plugZ: 62.4,
  box: 'Main_Box_Dock_0', plate: 'Plate_PCB_foam_DOCK_2', pcb: ['metal', 'tin', 'tan', 'black'].map(k => 'pcb_' + k),
};
export const DM = {
  body: () => new THREE.MeshPhysicalMaterial({color: 0x0b3550, roughness: .42, clearcoat: .55, clearcoatRoughness: .3}),
  plate: () => new THREE.MeshPhysicalMaterial({color: 0x0e8391, roughness: .4, clearcoat: .4}),
  pcb: () => new THREE.MeshStandardMaterial({color: 0x1d2326, roughness: .6}),
};

// TCOLD stands in a slot on its left edge (its USB-C port) with the screen facing -X:
// device +X -> world +Y, device +Y -> world -Z, device +Z (front) -> world -X.
// The port sits 20 mm below the device centre, so the centre is 20 mm on from the plug.
export function seat(dev, slot) {
  const m = new THREE.Matrix4().makeBasis(new THREE.Vector3(0, 1, 0), new THREE.Vector3(0, 0, -1), new THREE.Vector3(-1, 0, 0));
  dev.quaternion.setFromRotationMatrix(m);
  dev.position.set(DOCK.slotX[slot], DOCK.floorY + 43.5, DOCK.plugZ - 20);
}

// front print: the TCOLD DOCK Station logo (brand/dock_logo.py) on the navy front face (Z min)
async function dockPrint(parent) {
  const img = new Image(); img.src = 'brand/TCOLD_DOCK_STATION_logo_on_dark.svg'; await img.decode();
  const c = document.createElement('canvas'); c.width = 2400; c.height = Math.round(2400 * img.height / img.width); const x = c.getContext('2d');
  x.drawImage(img, 0, 0, c.width, c.height);
  const w = 66, h = w * c.height / c.width;
  const m = new THREE.Mesh(new THREE.PlaneGeometry(w, h), new THREE.MeshPhysicalMaterial({map: canvasTex(c), transparent: true, roughness: .4, clearcoat: .5, polygonOffset: true, polygonOffsetFactor: -2}));
  m.rotation.y = Math.PI; m.position.set(-21.1, -2, -17.35); parent.add(m);
}

// the dock, optionally holding n devices (charging screens D1..D6, side light green)
export async function dock(parent, {n = 6} = {}) {
  const g = new THREE.Group(); parent.add(g);
  mesh(await loadDock(DOCK.box), DM.body(), g);
  mesh(await loadDock(DOCK.plate), DM.plate(), g);
  for (const p of DOCK.pcb) mesh(await loadDock(p), DM.pcb(), g, false);
  await dockPrint(g);
  const devs = [];
  for (let i = 0; i < n; i++) {
    const d = await device(g, {screen: 'screen_D' + (i + 1), lit: [null, null, null], side: 0x3fd47a});
    seat(d, i); devs.push(d);
  }
  return {group: g, devices: devs};
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
