// Adding a box over USB, in the browser: Web Serial + esptool-js.
//
// One session per box: connect to the ROM (the box's USB-Serial-JTAG), read
// its MAC, ask the server for its SN and the images (the firmware's and an
// NVS image with its settings), erase, write, reset. The server records the
// result. In the demo the port is simulated by the server.
import { ESPLoader, Transport } from "./vendor/esptool-js-0.5.7.js";

export class NeedRom extends Error {}

const STEPS = [
  ["port", "เลือกพอร์ต USB"],
  ["connect", "เชื่อมต่อ อ่าน MAC"],
  ["prepare", "จอง SN เตรียมไฟล์"],
  ["download", "ดาวน์โหลด firmware + ค่าตั้ง"],
  ["erase", "ลบทั้งชิป"],
  ["write", "เขียน"],
  ["reset", "รีสตาร์ทกล่อง"],
  ["record", "บันทึกผล"],
];
export { STEPS };

async function api(path, body) {
  const r = await fetch(path, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body || {}) });
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.error || `${path}: ${r.status}`);
  return j;
}

async function download(prep, ui) {
  const files = [];
  let n = 0;
  for (const im of prep.images) {
    const r = await fetch(im.url, { cache: "no-store" });
    if (!r.ok) throw new Error(`ดาวน์โหลด ${im.name} ไม่ได้ (${r.status})`);
    const data = new Uint8Array(await r.arrayBuffer());
    if (data.length !== im.size) throw new Error(`${im.name}: ได้ ${data.length} ไบต์ ไม่ใช่ ${im.size}`);
    files.push({ name: im.name, address: im.offset, data });
    ui.step("download", `${++n}/${prep.images.length}`);
  }
  return files;
}

function binaryString(u8) {       // esptool-js 0.5 takes images as binary strings
  let s = "";
  for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000));
  return s;
}

/**
 * Flash one box. `form` = {lot, unit, wifi, erase_all, operator}; `ui` gets
 * step(name, note), log(line), progress(fraction). Returns the server's record.
 */
export async function addBox(cfg, form, ui) {
  return cfg.demo ? addDemo(cfg, form, ui) : addReal(cfg, form, ui);
}

async function finish(prep, ok, log, error) {
  return api("/api/usb/done", { token: prep.token, ok, log: log.join("\n"), error: error || "" });
}

async function addReal(cfg, form, ui) {
  if (!("serial" in navigator)) throw new Error("เบราว์เซอร์นี้ต่อ USB ไม่ได้: ใช้ Chrome/Edge ผ่าน https");
  const log = [];
  const say = (s) => { log.push(s); ui.log(s); };
  ui.step("port");
  const port = await navigator.serial.requestPort({ filters: [{ usbVendorId: cfg.usb.vid }] });
  const info = port.getInfo();
  if (cfg.usb.app_pids.includes(info.usbProductId)) {
    // the product firmware's own port: its console sends it to the ROM (no BOOT button)
    await port.open({ baudRate: cfg.usb.baud });
    const w = port.writable.getWriter();
    await w.write(new TextEncoder().encode(cfg.usb.download_command + "\r\n"));
    w.releaseLock();
    try { await port.close(); } catch (e) { /* it is gone already */ }
    throw new NeedRom("กล่องนี้มี firmware อยู่แล้ว สั่งเข้า download mode ให้แล้ว: กด เพิ่ม อีกครั้ง แล้วเลือกพอร์ต USB JTAG");
  }
  const transport = new Transport(port, false);
  let prep = null;
  try {
    ui.step("connect");
    const loader = new ESPLoader({ transport, baudrate: 921600, romBaudrate: 115200,
      terminal: { clean() {}, writeLine: say, write: say } });
    const chip = await loader.main();
    const mac = await loader.chip.readMac(loader);
    say(`chip ${chip}, MAC ${mac}`);
    ui.step("prepare");
    prep = await api("/api/usb/prepare", { mac, chip, lot: form.lot, unit: form.unit, wifi: form.wifi,
                                           erase_all: form.erase_all, operator: form.operator });
    ui.prepared(prep);
    ui.step("download");
    const files = await download(prep, ui);
    if (prep.erase_all) {
      ui.step("erase");
      await loader.eraseFlash();
    } else ui.step("erase", "ข้าม");
    ui.step("write");
    const total = files.reduce((a, f) => a + f.data.length, 0);
    const before = files.map((f, i) => files.slice(0, i).reduce((a, g) => a + g.data.length, 0));
    await loader.writeFlash({
      fileArray: files.map((f) => ({ data: binaryString(f.data), address: f.address })),
      flashSize: cfg.flash.size, flashMode: cfg.flash.mode, flashFreq: cfg.flash.freq,
      eraseAll: false, compress: true,
      reportProgress: (i, written) => ui.progress((before[i] + written) / total),
    });
    ui.step("reset");
    await loader.after("hard_reset");
    ui.step("record");
    return await finish(prep, true, log);
  } catch (e) {
    say(String(e && e.message || e));
    if (prep) { try { await finish(prep, false, log, String(e && e.message || e)); } catch (x) { /* shown below */ } }
    throw e;
  } finally {
    try { await transport.disconnect(); } catch (e) { /* closed */ }
  }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function addDemo(cfg, form, ui) {
  const log = [];
  const say = (s) => { log.push(s); ui.log(s); };
  ui.step("port"); await sleep(250);
  ui.step("connect");
  const port = await api("/api/demo/usb/connect");
  say(`chip ${port.chip}, MAC ${port.mac} (จำลอง)`);
  await sleep(300);
  ui.step("prepare");
  const prep = await api("/api/usb/prepare", { mac: port.mac, chip: port.chip, lot: form.lot, unit: form.unit,
                                               wifi: form.wifi, erase_all: form.erase_all, operator: form.operator });
  ui.prepared(prep);
  try {
    ui.step("download");
    const files = await download(prep, ui);
    ui.step("erase", prep.erase_all ? "" : "ข้าม"); await sleep(prep.erase_all ? 700 : 100);
    ui.step("write");
    for (let p = 0; p <= 1.0001; p += 0.1) { ui.progress(Math.min(p, 1)); await sleep(90); }
    await api("/api/demo/usb/write", { token: prep.token, ok: true });
    say(`wrote ${files.length} files (${files.map((f) => f.name).join(", ")})`);
    ui.step("reset"); await sleep(200);
    ui.step("record");
    return await finish(prep, true, log);
  } catch (e) {
    await finish(prep, false, log, String(e.message || e)).catch(() => {});
    throw e;
  }
}

/**
 * Factory mode back on, on a box shipped already: its console over the USB cable (the
 * product firmware's own port, 303A:4003), `factory on`, wait for `factory mode on`.
 * Only the cable can do it: over MQTT the box takes no write command outside factory mode.
 */
export async function factoryOn(cfg, sn) {
  if (cfg.demo) return api("/api/demo/usb/factory_on", { sn });
  if (!("serial" in navigator)) throw new Error("เบราว์เซอร์นี้ต่อ USB ไม่ได้: ใช้ Chrome/Edge ผ่าน https");
  const port = await navigator.serial.requestPort({ filters: [{ usbVendorId: cfg.usb.vid }] });
  if (!cfg.usb.app_pids.includes(port.getInfo().usbProductId))
    throw new Error("เลือกพอร์ต console ของกล่อง (mCOLD) ไม่ใช่ USB JTAG: กล่องต้องรัน firmware อยู่");
  await port.open({ baudRate: cfg.usb.baud });
  const w = port.writable.getWriter();
  const r = port.readable.getReader();
  let seen = "";
  try {
    await w.write(new TextEncoder().encode("factory on\r\n"));
    const until = Date.now() + 4000;
    const dec = new TextDecoder();
    while (Date.now() < until && !/factory mode on/.test(seen)) {
      const chunk = await Promise.race([r.read(), sleep(until - Date.now()).then(() => ({ done: true }))]);
      if (chunk.done) break;
      seen += dec.decode(chunk.value);
    }
  } finally {
    try { await r.cancel(); } catch (e) { /* closing */ }
    r.releaseLock(); w.releaseLock();
    try { await port.close(); } catch (e) { /* gone */ }
  }
  if (!/factory mode on/.test(seen)) throw new Error("กล่องไม่ตอบ factory mode on: " + (seen.trim().slice(-120) || "(เงียบ)"));
  return { ok: true, reply: "factory mode on" };
}
