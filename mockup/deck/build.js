// TCOLD presentation draft: three sections (product, function, specification) on a plain
// TCOLD theme, so the slides can be pasted into the house template and restyled there.
// Text uses the theme fonts only, so it takes the destination theme's fonts on paste.
//
// usage: node mockup/deck/build.js     (after python mockup/objects.py)
//        -> mockup/out/TCOLD_presentation.pptx
const path = require('path'), fs = require('fs');
const pptxgen = require('pptxgenjs');
const sharp = require('sharp');

const HERE = __dirname, MOCK = path.join(HERE, '..'), OBJ = path.join(MOCK, 'out/objects');
const ICONS = path.join(MOCK, '../mcold-spec/icons');
const OUT = path.join(MOCK, 'out/TCOLD_presentation.pptx');

const THEME = {
  name: 'TCOLD', headFontFace: 'Leelawadee UI', bodyFontFace: 'Leelawadee UI',
  colors: {dk1: '14293A', lt1: 'FFFFFF', dk2: '0B3550', lt2: 'EEF5F7',
    accent1: '0E8391', accent2: '1AA5B5', accent3: 'C63737', accent4: 'E0A21A', accent5: '22A9D6', accent6: '5F707C',
    hlink: '0E8391', folHlink: '12506F'},
};
const HEX = THEME.colors;

// PNG size from the IHDR header, for fitting images without distortion
const pngSize = f => { const b = fs.readFileSync(f); return {w: b.readUInt32BE(16), h: b.readUInt32BE(20)}; };
const obj = n => path.join(OBJ, n + '.png');

const iconCache = {};
async function icon(name, hex) {
  const k = name + hex;
  if (!iconCache[k]) {
    const svg = fs.readFileSync(path.join(ICONS, name + '.svg'), 'utf8').replace(/currentColor/g, '#' + hex);
    iconCache[k] = 'image/png;base64,' + (await sharp(Buffer.from(svg), {density: 900}).resize(256, 256).png().toBuffer()).toString('base64');
  }
  return iconCache[k];
}

// pptxgenjs writes Office's stock colour scheme; put the TCOLD colours into the theme part
// so scheme colours on the slides resolve to them (and follow a later theme change)
async function writeTheme(file, {name, colors, bodyFontFace: font}) {
  const JSZip = require('jszip');
  const zip = await JSZip.loadAsync(fs.readFileSync(file)), part = 'ppt/theme/theme1.xml';
  const slots = ['dk1', 'lt1', 'dk2', 'lt2', 'accent1', 'accent2', 'accent3', 'accent4', 'accent5', 'accent6', 'hlink', 'folHlink'];
  const scheme = `<a:clrScheme name="${name}">` + slots.map(k => `<a:${k}><a:srgbClr val="${colors[k]}"/></a:${k}>`).join('') + '</a:clrScheme>';
  let xml = await zip.file(part).async('string');
  // Thai text runs use the complex-script font: pptxgenjs leaves it empty and maps Thai to
  // Angsana New, so set both to the theme font
  xml = xml.replace(/<a:cs typeface="[^"]*"\/>/g, `<a:cs typeface="${font}"/>`).replace(/<a:font script="Thai" typeface="[^"]*"\/>/g, `<a:font script="Thai" typeface="${font}"/>`);
  xml = xml.replace(/<a:clrScheme\b[\s\S]*?<\/a:clrScheme>/, () => scheme).replace(/(<a:(?:theme|fontScheme)\b[^>]*?\bname=")[^"]*"/g, (_, h) => `${h}${name}"`);
  if (!xml.includes(scheme)) throw new Error('no colour scheme in ' + part);
  for (const n of Object.keys(zip.files)) {   // a scheme name given to a hex-only option is not a colour
    if (!n.endsWith('.xml')) continue;
    const bad = (await zip.file(n).async('string')).match(/<a:srgbClr val="((?![0-9A-Fa-f]{6}")[^"]*)"/);
    if (bad) throw new Error(`${n}: srgbClr "${bad[1]}" is not hex`);
  }
  zip.file(part, xml);
  fs.writeFileSync(file, await zip.generateAsync({type: 'nodebuffer', compression: 'DEFLATE'}));
}

(async () => {
  const pres = new pptxgen();
  pres.layout = 'LAYOUT_WIDE';                  // 13.333 x 7.5 in
  pres.title = 'TCOLD — Thanes Cold-chain Online Logging Device';
  pres.company = 'THANES';
  pres.theme = {headFontFace: THEME.headFontFace, bodyFontFace: THEME.bodyFontFace};
  const C = pres.SchemeColor;
  const W = 13.333, M = 0.6;

  // ---------- layouts ----------
  pres.defineSlideMaster({
    title: 'COVER', background: {color: C.text2},
    objects: [
      {placeholder: {options: {name: 'kicker', type: 'body', x: M, y: 1.0, w: 6.4, h: 0.4, fontSize: 14, bold: true, color: C.accent2, charSpacing: 4}, text: ''}},
      {placeholder: {options: {name: 'title', type: 'title', x: M, y: 3.55, w: 6.4, h: 1.2, align: 'left', fontSize: 30, bold: true, color: C.background1, valign: 'top'}, text: ''}},
      {placeholder: {options: {name: 'body', type: 'body', x: M, y: 4.75, w: 6.0, h: 1.1, fontSize: 16, color: 'CFE3EA', valign: 'top'}, text: ''}},
    ],
  });
  pres.defineSlideMaster({
    title: 'SECTION', background: {color: C.text2},
    objects: [
      {placeholder: {options: {name: 'num', type: 'body', x: M, y: 2.0, w: 3, h: 1.3, fontSize: 72, bold: true, color: C.accent2}, text: ''}},
      {placeholder: {options: {name: 'title', type: 'title', x: M, y: 3.3, w: 8, h: 1.0, align: 'left', fontSize: 40, bold: true, color: C.background1}, text: ''}},
      {placeholder: {options: {name: 'body', type: 'body', x: M, y: 4.3, w: 7, h: 1.2, fontSize: 18, color: 'CFE3EA', valign: 'top'}, text: ''}},
      {image: {x: W - M - 2.2, y: 6.65, w: 2.2, h: 2.2 * 409 / 2331, path: obj('TCOLD_logo_on_dark')}},
    ],
  });
  pres.defineSlideMaster({
    title: 'CONTENT', background: {color: C.background1},
    objects: [
      {placeholder: {options: {name: 'kicker', type: 'body', x: M, y: 0.35, w: 8, h: 0.35, fontSize: 12, bold: true, color: C.accent1, charSpacing: 3, margin: 0}, text: ''}},
      {placeholder: {options: {name: 'title', type: 'title', x: M, y: 0.68, w: W - 2 * M, h: 0.8, align: 'left', fontSize: 32, bold: true, color: C.text2, margin: 0, valign: 'top'}, text: ''}},
      {image: {x: W - M - 1.3, y: 6.95, w: 1.3, h: 1.3 * 409 / 2331, path: obj('TCOLD_logo')}},
    ],
    slideNumber: {x: M, y: 6.92, w: 0.6, h: 0.3, fontSize: 10, color: C.accent6, margin: 0},
  });

  // ---------- helpers ----------
  let section = '';
  const addSection = t => { section = t; pres.addSection({title: t}); };
  const content = (kicker, title) => {
    const s = pres.addSlide({masterName: 'CONTENT', sectionTitle: section});
    s.addText(kicker, {placeholder: 'kicker'}); s.addText(title, {placeholder: 'title'}); return s;
  };
  // fit an image inside a box, centred
  const pic = (s, name, x, y, w, h, opts = {}) => {
    const {w: pw, h: ph} = pngSize(obj(name)); const k = Math.min(w / pw, h / ph);
    const iw = pw * k, ih = ph * k;
    s.addImage({path: obj(name), x: x + (w - iw) / 2, y: y + (h - ih) / 2, w: iw, h: ih, altText: opts.alt || name, objectName: name});
    return {x: x + (w - iw) / 2, y: y + (h - ih) / 2, w: iw, h: ih};
  };
  const badge = async (s, name, x, y, d = 0.56, fill = C.accent1, fg = 'FFFFFF') => {
    s.addShape(pres.shapes.OVAL, {x, y, w: d, h: d, fill: {color: fill}, line: {type: 'none'}, objectName: 'icon ' + name});
    s.addImage({data: await icon(name, fg), x: x + d * 0.22, y: y + d * 0.22, w: d * 0.56, h: d * 0.56, altText: name});
  };
  const txt = (s, text, o) => s.addText(text, {isTextBox: true, margin: 0, valign: 'top', fontSize: 14, color: C.text1, lang: 'th-TH', ...o});
  const card = (s, x, y, w, h, fill = C.background2) => s.addShape(pres.shapes.ROUNDED_RECTANGLE, {x, y, w, h, rectRadius: 0.15, fill: {color: fill}, line: {type: 'none'}});
  // icon row: badge, bold head, description
  const iconRow = async (s, x, y, w, ic, head, desc, {d = 0.56, fs = 15} = {}) => {
    await badge(s, ic, x, y, d);
    txt(s, [{text: head, options: {bold: true, color: C.text2, fontSize: fs + 2, breakLine: true}}, {text: desc, options: {color: C.accent6, fontSize: fs}}],
      {x: x + d + 0.25, y: y - 0.02, w: w - d - 0.25, h: 0.9});
  };

  // =====================================================================
  // cover
  {
    const s = pres.addSlide({masterName: 'COVER'});
    s.addText('THANES · SMART COLD CHAIN', {placeholder: 'kicker'});
    s.addImage({path: obj('TCOLD_lockup_on_dark'), x: M, y: 1.65, w: 5.6, h: 5.6 * 540 / 2331, altText: 'TCOLD — Thanes Cold-chain Online Logging Device'});
    s.addText('อุปกรณ์ติดตามอุณหภูมิกล่องขนส่งยาและเวชภัณฑ์', {placeholder: 'title'});
    s.addText('บันทึกทุก 5 นาที แจ้งเตือนเมื่อออกนอกช่วง ส่งข้อมูลขึ้นระบบอัตโนมัติ ตลอดเส้นทางขนส่ง', {placeholder: 'body'});
    // the device on a soft light panel so its navy shadow reads on the dark cover
    s.addShape(pres.shapes.ROUNDED_RECTANGLE, {x: 7.25, y: 0.75, w: 5.5, h: 6.0, rectRadius: 0.3, fill: {color: 'F2F8FA'}, line: {type: 'none'}});
    pic(s, 'device_with_probe', 7.45, 1.0, 5.1, 5.5);
    s.addNotes('TCOLD คืออุปกรณ์ติดกล่องโฟมขนส่งยาและเวชภัณฑ์ วัดอุณหภูมิด้วยหัววัด Thermocouple บันทึกทุก 5 นาที และแจ้งเตือนเมื่ออุณหภูมิออกนอกช่วงที่กำหนดของแต่ละทริป');
  }

  // =====================================================================
  addSection('หน้าตาโปรดักต์');
  {
    const s = pres.addSlide({masterName: 'SECTION', sectionTitle: section});
    s.addText('01', {placeholder: 'num'}); s.addText('หน้าตาโปรดักต์', {placeholder: 'title'});
    s.addText('ตัวเครื่อง หัววัด ส่วนประกอบภายใน และแอป', {placeholder: 'body'});
  }
  {
    const s = content('01 · PRODUCT', 'TCOLD ติดไปกับกล่อง ดูแลอุณหภูมิตลอดทริป');
    pic(s, 'device_with_probe', 0.4, 1.55, 7.2, 5.2);
    const x = 7.9, w = 4.85;
    await iconRow(s, x, 1.95, w, 'temperature', 'วัดและบันทึกทุก 5 นาที', 'หัววัด Thermocouple Type K วางในกล่อง ตัวเครื่องติดด้านนอก อ่านค่าได้จากจอทันที');
    await iconRow(s, x, 3.35, w, 'bell-ringing', 'เตือนทันทีเมื่อออกนอกช่วง', 'เสียง ไฟ และจอเปลี่ยนเป็นสีแดง พร้อมส่งแจ้งเตือนขึ้นระบบ');
    await iconRow(s, x, 4.75, w, 'shield-check', 'หลักฐานครบทั้งทริป', 'เก็บอุณหภูมิ ตำแหน่ง การเปิดฝา และการกระแทก ไม่หายแม้ไม่มีสัญญาณ');
    s.addNotes('ภาพรวมของสินค้า: ตัวเครื่องติดกับกล่อง หัววัดอยู่ในกล่อง ผู้ใช้ไม่ต้องทำอะไรระหว่างขนส่ง');
  }
  {
    const s = content('01 · PRODUCT', 'รอบตัวเครื่อง');
    const f = pic(s, 'device_front', 3.95, 1.5, 5.4, 5.3);
    // callouts left and right of the front view; x positions keep clear of the device
    const L = [[1.75, 'จุดแตะ NFC', 'อยู่บริเวณโลโก้ แตะมือถือเพื่อเชื่อมต่อแอป'],
               [3.45, 'จอ e-Paper 2.13″', 'อุณหภูมิล่าสุด MIN/MAX และสถานะ ภาพค้างได้โดยไม่ใช้ไฟ'],
               [5.05, 'ไฟสถานะ 3 ดวง', 'ซ้าย สินค้า · กลาง ทริป · ขวา ตัวเครื่อง']];
    for (const [y, h, d] of L) txt(s, [{text: h, options: {bold: true, color: C.text2, fontSize: 17, breakLine: true}}, {text: d, options: {color: C.accent6, fontSize: 14}}],
      {x: M - 0.2, y, w: 3.55, h: 1.1, align: 'right'});
    pic(s, 'device_back', 9.6, 1.55, 3.0, 2.6);
    txt(s, [{text: 'ด้านหลัง', options: {bold: true, color: C.text2, fontSize: 17, breakLine: true}}, {text: 'ฝาหลังสีเทอร์ควอยซ์ ติดกับฝากล่องโฟม', options: {color: C.accent6, fontSize: 14}}],
      {x: 9.6, y: 4.3, w: 3.2, h: 0.9});
    txt(s, [{text: '87 × 87 × 12.3 มม.', options: {bold: true, color: C.accent1, fontSize: 22, breakLine: true}}, {text: 'ขนาดตัวเครื่อง วัดจากแบบ 3D', options: {color: C.accent6, fontSize: 14}}],
      {x: 9.75, y: 5.35, w: 3.0, h: 0.9});
    s.addNotes('ด้านหน้ามีโลโก้ซึ่งเป็นตำแหน่งเสา NFC จอ e-Paper และไฟสถานะ 3 ดวง สีของไฟแต่ละตำแหน่งคงที่ แดงซ้ายคือสินค้า เหลืองขวาคือตัวเครื่อง');
  }
  {
    const s = content('01 · PRODUCT', 'หัววัด Thermocouple Type K');
    card(s, M, 1.65, W - 2 * M, 3.0, 'F4F8FA');
    pic(s, 'thermocouple', M + 0.3, 1.8, W - 2 * M - 0.6, 2.7);
    const cols = [['temperature', 'ปลอกสแตนเลส', 'สอดเข้ากล่องโฟม วัดอุณหภูมิของสินค้าโดยตรง'],
                  ['activity', 'สายถักโลหะ', 'ทนการพับและเสียดสีระหว่างขนย้าย'],
                  ['chart-line', 'ความละเอียด 0.25 °C', 'แปลงสัญญาณด้วย MAX6675 เหมาะกับช่วง 2–8 °C'],
                  ['alert-triangle', 'รู้ทันทีถ้าหลุด', 'จอแสดง -- สีแดง ไฟขวาเหลือง ไม่แสดงค่าผิดเป็น 0 °C']];
    const cw = (W - 2 * M - 0.9) / 4;
    for (let i = 0; i < 4; i++) await iconRow(s, M + i * (cw + 0.3), 5.1, cw, cols[i][0], cols[i][1], cols[i][2], {fs: 13});
    s.addNotes('หัววัดอ่านได้ตั้งแต่ 0 °C ขึ้นไป จึงเหมาะกับยาและเวชภัณฑ์ช่วง 2–8 °C ความละเอียด 0.25 °C ไม่ใช่ความแม่นยำ ความแม่นยำขึ้นกับผลสอบเทียบ');
  }
  {
    const s = content('01 · PRODUCT', 'ภายในเครื่อง');
    pic(s, 'explode_stack', 0.4, 1.45, W - 0.8, 3.55);
    const parts = [['1', 'เคสหน้า', 'ช่องจอ + รูไฟ'], ['2', 'ท่อนำแสง', 'ไฟ RGB 3 ดวง'], ['3', 'จอ e-Paper', '2.13″ 250×122'], ['4', 'บอร์ด NFC', 'ST25DV04KC'],
                   ['5', 'แผ่นรองจอ', 'ยึดจอและ NFC'], ['6', 'บอร์ดหลัก', 'ESP32-S3 · GNSS'], ['7', 'แบตเตอรี่', 'Li-Po 59×37×5'], ['8', 'เคสหลัง', 'หนารวม 12.3 มม.']];
    const cw = (W - 2 * M - 0.7 * 3) / 4;
    parts.forEach(([n, h, d], i) => {
      const x = M + (i % 4) * (cw + 0.7), y = 5.15 + Math.floor(i / 4) * 0.82;
      s.addShape(pres.shapes.OVAL, {x, y: y + 0.02, w: 0.42, h: 0.42, fill: {color: C.text2}, line: {type: 'none'}});
      txt(s, n, {x, y: y + 0.02, w: 0.42, h: 0.42, align: 'center', valign: 'middle', bold: true, color: C.background1, fontSize: 14});
      txt(s, [{text: h, options: {bold: true, color: C.text2, fontSize: 15, breakLine: true}}, {text: d, options: {color: C.accent6, fontSize: 12}}],
        {x: x + 0.55, y, w: cw - 0.55, h: 0.75});
    });
    s.addNotes('ส่วนประกอบเรียงจากหน้าไปหลัง (ซ้ายไปขวาในภาพ) ภาพแยกชิ้นแต่ละชิ้นอยู่ในโฟลเดอร์ objects ชื่อ part_1 ถึง part_8');
  }
  {
    const s = content('01 · PRODUCT', 'แอป TCOLD');
    pic(s, 'phone_app_tilted', 8.3, 1.3, 4.4, 5.55);
    const rows = [['nfc', 'แตะเพื่อเชื่อมต่อ', 'แตะมือถือที่เครื่อง แอปเชื่อมต่อผ่าน Bluetooth เอง ไม่ต้องค้นหาเครื่อง'],
                  ['chart-line', 'ดูอุณหภูมิสดและกราฟทั้งทริป', 'ค่าล่าสุด ต่ำสุด/สูงสุด จำนวน alarm แบต และพื้นที่เก็บข้อมูล'],
                  ['player-play', 'เริ่ม / หยุดทริป', 'กรอกข้อมูลงานและช่วงอุณหภูมิ เช่น 2–8 °C แล้วกดเริ่ม'],
                  ['bell-ringing', 'รับทราบ alarm', 'หยุดเสียงเตือนที่กล่อง ประวัติการแจ้งเตือนยังอยู่ครบ'],
                  ['file-spreadsheet', 'ส่งออก CSV', 'เปิดใน Excel ได้ทันที']];
    for (let i = 0; i < rows.length; i++) await iconRow(s, M, 1.65 + i * 1.0, 7.3, ...rows[i], {d: 0.5, fs: 14});
    s.addNotes('หน้าจอในภาพเป็นภาพแนวคิดของแอป ตัวเลขตรงกับหน้าจอบนเครื่อง');
  }
  {
    const s = content('01 · PRODUCT', 'TCOLD Dock ชาร์จ 6 เครื่องพร้อมกัน');
    pic(s, 'dock_with_6', 5.9, 1.35, 5.2, 5.0);
    pic(s, 'dock_back', 10.85, 4.55, 1.9, 1.75);
    txt(s, [{text: 'ด้านหลัง: USB-C 2 ช่อง', options: {bold: true, color: C.text2, fontSize: 12, breakLine: true}}, {text: 'ไฟเข้า PD · ต่อคอมพิวเตอร์', options: {color: C.accent6, fontSize: 11}}],
      {x: 10.6, y: 6.3, w: 2.4, h: 0.55, align: 'center'});
    const rows = [['plug', 'วางลงช่องแล้วชาร์จทันที', 'ขั้ว USB-C ที่ก้นช่องเสียบเข้าพอร์ตของเครื่องเอง ไฟข้างเครื่องเขียว = กำลังชาร์จ'],
                  ['file-spreadsheet', 'ดึงไฟล์ CSV ทุกเครื่อง', 'ต่อคอมพิวเตอร์สายเดียว เห็นแต่ละเครื่องเป็นไดรฟ์แยก'],
                  ['shield-check', 'ป้องกันทุกช่อง', 'ตัดกระแสเกินแยกช่อง · TVS และ eFuse ที่ขาเข้า · ไม่มี firmware ให้ดูแล']];
    for (let i = 0; i < rows.length; i++) await iconRow(s, M, 1.75 + i * 1.45, 5.0, ...rows[i], {d: 0.56, fs: 14});
    s.addNotes('Dock ไม่มี MCU และไม่มีไฟบนตัว Dock สถานะชาร์จดูจากไฟข้างของแต่ละเครื่อง ต้องเสียบ adapter USB-C PD เสมอ ใช้ไฟจากคอมพิวเตอร์อย่างเดียวไม่พอ');
  }

  // =====================================================================
  addSection('FUNCTION');
  {
    const s = pres.addSlide({masterName: 'SECTION', sectionTitle: section});
    s.addText('02', {placeholder: 'num'}); s.addText('FUNCTION', {placeholder: 'title'});
    s.addText('เครื่องทำอะไรได้ และใช้งานอย่างไรตั้งแต่ต้นทางถึงปลายทาง', {placeholder: 'body'});
  }
  {
    const s = content('02 · FUNCTION', 'ฟังก์ชันหลัก');
    const F = [['temperature', 'ติดตามอุณหภูมิ', 'Type K ทุก 5 นาที (ปรับได้)'], ['bell-ringing', 'แจ้งเตือนหลายช่องทาง', 'เสียง ไฟ จอ และขึ้นระบบ'],
               ['door', 'ตรวจจับการเปิดฝา', 'บันทึกเวลาเปิด-ปิดทุกครั้ง'], ['activity', 'ตรวจจับการกระแทก', 'เซ็นเซอร์ความเร่ง 3 แกน'],
               ['map-pin', 'ระบุตำแหน่ง GNSS', 'บันทึกพิกัดระหว่างขนส่ง'], ['eye', 'จอ e-Paper 2.13″', 'ภาพค้างได้โดยไม่ใช้ไฟ'],
               ['nfc', 'แตะเพื่อเริ่มงาน', 'NFC + แอปบนมือถือ'], ['cloud-upload', 'ส่งข้อมูลอัตโนมัติ', 'Wi-Fi 2.4 GHz · MQTT'],
               ['database', 'เก็บเมื่อไม่มีสัญญาณ', 'ประมาณ 268 วันในเครื่อง'], ['usb', 'ดึงไฟล์ผ่าน USB', 'เห็นเป็นไดรฟ์ ไฟล์ CSV'],
               ['battery-charging', 'Dock ชาร์จ 6 เครื่อง', 'ชาร์จพร้อมกัน USB-C PD'], ['battery-3', 'ใช้งานต่อเนื่อง 7 วัน', 'เป้าหมายการออกแบบ']];
    const cols = 4, gx = 0.3, gy = 0.3, cw = (W - 2 * M - gx * (cols - 1)) / cols, ch = 1.45;
    for (let i = 0; i < F.length; i++) {
      const x = M + (i % cols) * (cw + gx), y = 1.65 + Math.floor(i / cols) * (ch + gy);
      card(s, x, y, cw, ch);
      await badge(s, F[i][0], x + 0.25, y + 0.25, 0.5);
      txt(s, [{text: F[i][1], options: {bold: true, color: C.text2, fontSize: 15, breakLine: true}}, {text: F[i][2], options: {color: C.accent6, fontSize: 13}}],
        {x: x + 0.25, y: y + 0.82, w: cw - 0.4, h: 0.6});
    }
    s.addNotes('ฟังก์ชันทั้งหมดอยู่ในแผนพัฒนา firmware ของเครื่องต้นแบบ ใช้งานต่อเนื่อง 7 วันเป็นค่าเป้าหมาย ยังไม่ได้วัดจริง');
  }
  {
    const s = content('02 · FUNCTION', 'ขั้นตอนการใช้งาน');
    const st = [['battery-charging', 'เตรียมเครื่อง', 'ชาร์จใน Dock เปิดสวิตช์ จอแสดง READY'],
                ['hand-finger', 'เริ่มทริป', 'แตะมือถือ กรอกข้อมูลงาน กดเริ่ม'],
                ['truck-delivery', 'ระหว่างขนส่ง', 'เครื่องวัด บันทึก และส่งข้อมูลเอง'],
                ['alert-triangle', 'เมื่อผิดปกติ', 'เตือนทันที แตะมือถือเพื่อรับทราบ'],
                ['flag', 'ส่งมอบ / จบงาน', 'แตะมือถือ กดจบงาน จอแสดงสรุป'],
                ['building-warehouse', 'กลับคลัง', 'ชาร์จและดึงข้อมูล CSV']];
    const n = st.length, cw = (W - 2 * M) / n, cy = 2.35;
    s.addShape(pres.shapes.LINE, {x: M + cw / 2, y: cy + 0.45, w: cw * (n - 1), h: 0, line: {color: HEX.accent2, width: 2, dashType: 'dash'}});
    for (let i = 0; i < n; i++) {
      const cx = M + i * cw + cw / 2;
      await badge(s, st[i][0], cx - 0.45, cy, 0.9, i === 3 ? C.accent3 : C.accent1);
      txt(s, String(i + 1), {x: cx - 0.45, y: cy + 1.05, w: 0.9, h: 0.4, align: 'center', bold: true, color: C.accent1, fontSize: 14});
      txt(s, [{text: st[i][1], options: {bold: true, color: C.text2, fontSize: 17, breakLine: true}}, {text: st[i][2], options: {color: C.accent6, fontSize: 13}}],
        {x: M + i * cw + 0.1, y: cy + 1.45, w: cw - 0.2, h: 1.4, align: 'center'});
    }
    card(s, M, 5.55, W - 2 * M, 0.95);
    txt(s, [{text: 'ผู้ใช้แตะมือถือแค่ตอนเริ่มและจบงาน ', options: {bold: true, color: C.text2}}, {text: 'ระหว่างทางเครื่องทำงานเองทั้งหมด การแตะ NFC อย่างเดียวไม่เริ่มทริป ต้องกดยืนยันในแอปทุกครั้ง', options: {color: C.text1}}],
      {x: M + 0.3, y: 5.55, w: W - 2 * M - 0.6, h: 0.95, valign: 'middle', fontSize: 15});
    s.addNotes('ขั้นตอนตั้งแต่คลังต้นทางถึงกลับคลัง ผู้ใช้ทำแค่ 2 จุด คือแตะตอนเริ่มและตอนจบ');
  }
  {
    const s = content('02 · FUNCTION', 'เริ่มทริปด้วยการแตะ');
    pic(s, 'state_A1_ready', M, 1.6, 3.4, 3.1);
    s.addShape(pres.shapes.RIGHT_ARROW, {x: 4.15, y: 3.05, w: 0.6, h: 0.5, fill: {color: C.accent2}, line: {type: 'none'}});
    pic(s, 'phone_app', 4.85, 1.45, 2.3, 3.6);
    s.addShape(pres.shapes.RIGHT_ARROW, {x: 7.25, y: 3.05, w: 0.6, h: 0.5, fill: {color: C.accent2}, line: {type: 'none'}});
    pic(s, 'state_A2_trip', 7.95, 1.6, 3.4, 3.1);
    txt(s, 'READY · พร้อมเริ่ม', {x: M, y: 4.8, w: 3.4, h: 0.4, align: 'center', bold: true, color: C.text2, fontSize: 15});
    txt(s, 'แอป', {x: 4.85, y: 5.1, w: 2.3, h: 0.4, align: 'center', bold: true, color: C.text2, fontSize: 15});
    txt(s, 'TRIP ACTIVE · ไฟกลางฟ้า', {x: 7.95, y: 4.8, w: 3.4, h: 0.4, align: 'center', bold: true, color: C.text2, fontSize: 15});
    const steps = ['แตะมือถือที่จุด NFC ไฟหน้ากะพริบม่วง เครื่องเปิด Bluetooth 60 วินาที',
                   'แอปเชื่อมต่อเอง กรอกข้อมูลงานและช่วงอุณหภูมิ เช่น 2–8 °C',
                   'กดเริ่ม ไฟกลางกะพริบฟ้า 3 ครั้ง จอขึ้นไอคอนทริป'];
    steps.forEach((t, i) => {
      const y = 5.75;   // one row of three steps under the pictures
      const x = M + i * 4.1;
      s.addShape(pres.shapes.OVAL, {x, y, w: 0.38, h: 0.38, fill: {color: C.accent1}, line: {type: 'none'}});
      txt(s, String(i + 1), {x, y, w: 0.38, h: 0.38, align: 'center', valign: 'middle', bold: true, color: C.background1, fontSize: 13});
      txt(s, t, {x: x + 0.5, y: y - 0.04, w: 3.45, h: 0.85, fontSize: 13});
    });
    s.addNotes('ไม่ต้องจับคู่ Bluetooth หรือใส่รหัส การแตะ NFC ใช้ยืนยันสิทธิ์ เครื่องไม่ให้เริ่มทริปถ้าแบตต่ำหรือมีทริปอื่นทำงานอยู่');
  }
  {
    const s = content('02 · FUNCTION', 'การแจ้งเตือนอุณหภูมิ');
    // illustrative trip segment, every 5 min: crosses 8 °C, stays out for the dwell, comes back below 7.5 °C
    const t = [], v = [5.0, 5.2, 5.6, 6.3, 7.2, 8.1, 8.6, 9.1, 9.4, 9.2, 8.6, 8.0, 7.6, 7.2, 6.6, 6.0, 5.5, 5.2];
    for (let i = 0; i < v.length; i++) { const m = 10 * 60 + 15 + i * 5; t.push(i % 3 ? '' : `${Math.floor(m / 60)}:${String(m % 60).padStart(2, '0')}`); }
    s.addChart([
      {type: pres.charts.LINE, data: [{name: 'อุณหภูมิ', labels: t, values: v}], options: {chartColors: ['0E8391'], lineSize: 3, lineDataSymbol: 'circle', lineDataSymbolSize: 6}},
      {type: pres.charts.LINE, data: [{name: 'ขอบบน 8 °C', labels: t, values: v.map(() => 8)}, {name: 'ขอบล่าง 2 °C', labels: t, values: v.map(() => 2)}],
        options: {chartColors: ['C63737', '22A9D6'], lineSize: 1.5, lineDash: 'dash', lineDataSymbol: 'none'}},
    ], {x: M, y: 1.55, w: 7.6, h: 4.6, valAxisMinVal: 0, valAxisMaxVal: 10, valAxisMajorUnit: 2,
      valAxisTitle: '°C', showValAxisTitle: true, valAxisTitleFontSize: 11, valAxisTitleColor: '5F707C',
      catAxisLabelColor: '5F707C', valAxisLabelColor: '5F707C', catAxisLabelFontSize: 10, valAxisLabelFontSize: 10,
      catAxisLabelFontFace: '+mn-lt', valAxisLabelFontFace: '+mn-lt', legendFontFace: '+mn-lt', titleFontFace: '+mn-lt',
      valGridLine: {color: 'E3EAEE', size: 0.75}, catGridLine: {style: 'none'},
      showLegend: true, legendPos: 'b', legendFontSize: 11, legendColor: '5F707C',
      showTitle: true, title: 'ตัวอย่าง: อุณหภูมิในกล่องระหว่างเปิดฝานาน', titleFontSize: 13, titleColor: '0B3550'});
    txt(s, 'ข้อมูลตัวอย่างเพื่ออธิบายการทำงาน', {x: M, y: 6.2, w: 7.6, h: 0.3, fontSize: 10, color: C.accent6, italic: true});
    const R = [['เกิน 8 °C', 'จอเปลี่ยนตัวเลขเป็นสีแดง ยังไม่เตือน', 'state_A3_out_of_range'],
               ['นอกช่วงต่อเนื่อง 5 นาที', 'ALARM: บี๊บ 3 ครั้ง ไฟซ้ายกะพริบแดง จอกรอบแดง ส่งขึ้นระบบ', 'state_A4_alarm'],
               ['กลับเข้าช่วงลึก 0.5 °C', 'ต่ำกว่า 7.5 °C alarm หาย ประวัติยังอยู่ครบ', null]];
    const ok = await icon('circle-check', HEX.accent1);
    R.forEach(([h, d, im], i) => {
      const y = 1.6 + i * 1.6, x = 8.55;
      if (im) pic(s, im, x, y, 1.35, 1.35); else { card(s, x + 0.1, y + 0.1, 1.15, 1.15); s.addImage({data: ok, x: x + 0.38, y: y + 0.38, w: 0.6, h: 0.6}); }
      txt(s, [{text: h, options: {bold: true, color: i === 1 ? C.accent3 : C.text2, fontSize: 16, breakLine: true}}, {text: d, options: {color: C.accent6, fontSize: 13}}],
        {x: x + 1.5, y: y + 0.15, w: W - M - x - 1.5, h: 1.3});
    });
    s.addNotes('เวลาที่ต้องอยู่นอกช่วงก่อนเตือน (ค่าเริ่มต้น 5 นาที) และระยะกลับเข้าช่วง (0.5 °C) ตั้งได้ในแอป การรับทราบในแอปหยุดเสียง แต่ประวัติ alarm ไม่หาย');
  }
  {
    const s = content('02 · FUNCTION', 'ไฟสถานะและหน้าจอ');
    const leds = [['FF3B3B', 'ซ้าย · สินค้า', ['แดง 2 ครั้ง + บี๊บ = alarm อุณหภูมิ', 'น้ำเงิน 1 ครั้ง = รับทราบ alarm']],
                  ['22C8F0', 'กลาง · ทริป', ['ฟ้า 3 ครั้ง = เริ่มทริป', 'ฟ้าสั้นๆ ทุก 15 นาที = ทำงานปกติ', 'น้ำเงิน 1 ครั้ง = หยุดทริป']],
                  ['FFB020', 'ขวา · ตัวเครื่อง', ['เหลือง 3 ครั้ง = ต้องตรวจ', 'probe หลุด แบตต่ำ นาฬิกาไม่มีเวลา']]];
    const cw = (W - 2 * M - 0.6) / 3;
    leds.forEach(([hex, h, lines], i) => {
      const x = M + i * (cw + 0.3), y = 1.6;
      card(s, x, y, cw, 1.95);
      s.addShape(pres.shapes.OVAL, {x: x + 0.3, y: y + 0.3, w: 0.42, h: 0.42, fill: {color: hex}, line: {color: 'FFFFFF', width: 2}});
      txt(s, h, {x: x + 0.9, y: y + 0.32, w: cw - 1.1, h: 0.4, bold: true, color: C.text2, fontSize: 17});
      txt(s, lines.map((l, k) => ({text: l, options: {bullet: true, breakLine: k < lines.length - 1}})), {x: x + 0.3, y: y + 0.9, w: cw - 0.5, h: 1.0, fontSize: 13, paraSpaceAfter: 3});
    });
    const scr = [['state_A1_ready', 'READY', 'ยังไม่มีทริป'], ['state_A2_trip', 'TRIP ACTIVE', 'อุณหภูมิ + MIN/MAX'],
                 ['state_A4_alarm', 'ALARM', 'ตัวเลขแดง + กรอบแดง'], ['state_C1_summary', 'TRIP SUMMARY', 'สรุป 5 นาทีหลังจบงาน']];
    const sw = (W - 2 * M - 0.9) / 4;
    scr.forEach(([im, h, d], i) => {
      const x = M + i * (sw + 0.3);
      pic(s, im, x, 3.75, sw, 2.3);
      txt(s, [{text: h + '  ', options: {bold: true, color: i === 2 ? C.accent3 : C.text2, fontSize: 14}}, {text: d, options: {color: C.accent6, fontSize: 12}}],
        {x, y: 6.1, w: sw, h: 0.4, align: 'center'});
    });
    s.addNotes('สีแดงและเหลืองคือการเตือนเสมอ และอยู่ตำแหน่งเดิมทุกครั้ง น้ำเงิน ม่วง ฟ้า คือสถานะปกติ ไฟติดเป็นจังหวะสั้นเพื่อประหยัดแบต ไม่ได้ติดค้าง');
  }
  {
    const s = content('02 · FUNCTION', 'ข้อมูลไปไหน');
    const y = 1.9, gap = 0.75, bw = (W - 2 * M - 3 * gap) / 4;
    const nodes = [['device_front', 'TCOLD', 'บันทึกทุก 5 นาที'], ['wifi', 'Wi-Fi 2.4 GHz', 'MQTT · ส่งทันทีที่บันทึก'],
                   ['server', 'Server', 'เก็บถาวร ยืนยันกลับ (ACK)'], ['device-laptop', 'แอป / รายงาน', 'ดูย้อนหลังทั้งทริป']];
    for (let i = 0; i < nodes.length; i++) {
      const x = M + i * (bw + gap);
      card(s, x, y, bw, 2.2);
      if (i === 0) pic(s, 'device_front', x + (bw - 1.45) / 2, y + 0.1, 1.45, 1.35);
      else await badge(s, nodes[i][0], x + bw / 2 - 0.45, y + 0.3, 0.9);
      txt(s, [{text: nodes[i][1], options: {bold: true, color: C.text2, fontSize: 16, breakLine: true}}, {text: nodes[i][2], options: {color: C.accent6, fontSize: 12}}],
        {x: x + 0.15, y: y + 1.45, w: bw - 0.3, h: 0.75, align: 'center'});
      if (i < nodes.length - 1) s.addShape(pres.shapes.RIGHT_ARROW, {x: x + bw + 0.15, y: y + 0.85, w: gap - 0.3, h: 0.42, fill: {color: C.accent2}, line: {type: 'none'}});
    }
    const B = [['database', 'ไม่มีสัญญาณก็ไม่หาย', 'เก็บในเครื่องได้ประมาณ 268 วัน แล้วทยอยส่งเมื่อกลับมามี Wi-Fi ลบเฉพาะข้อมูลที่ server ยืนยันแล้ว'],
               ['usb', 'เสียบคอมพิวเตอร์', 'เห็นเป็น Flash drive คัดลอกไฟล์ CSV ไปเปิดใน Excel ไม่ต้องลงโปรแกรม'],
               ['battery-charging', 'Dock 6 ช่อง', 'ชาร์จพร้อมกัน 6 เครื่องด้วย adapter USB-C PD 45 W']];
    const cw = (W - 2 * M - 0.6) / 3;
    for (let i = 0; i < 3; i++) await iconRow(s, M + i * (cw + 0.3), 4.6, cw, ...B[i], {d: 0.5, fs: 13});
    s.addNotes('ข้อมูลทุกแถวบันทึกลงหน่วยความจำก่อน แล้วจึงส่ง ถ้า server ไม่ยืนยัน เครื่องจะเก็บไว้และส่งซ้ำ');
  }

  // =====================================================================
  addSection('SPECIFICATION');
  {
    const s = pres.addSlide({masterName: 'SECTION', sectionTitle: section});
    s.addText('03', {placeholder: 'num'}); s.addText('SPECIFICATION', {placeholder: 'title'});
    s.addText('ตัวเลขสำคัญ ขนาด และข้อมูลจำเพาะ', {placeholder: 'body'});
  }
  {
    const s = content('03 · SPECIFICATION', 'ตัวเลขสำคัญ');
    const K = [['temperature', '0.25 °C', 'ความละเอียดการวัด'], ['clock', '5 นาที', 'รอบบันทึกอุณหภูมิ (ตั้งได้ 5–60 นาที)'],
               ['database', '~268 วัน', 'เก็บข้อมูลในเครื่อง ที่รอบ 5 นาที'], ['battery-3', '7 วัน', 'ใช้งานต่อเนื่อง · เป้าหมาย'],
               ['device-tablet', '87 × 87 × 12.3', 'ขนาดตัวเครื่อง (มม.)'], ['eye', '2.13″', 'จอ e-Paper 250 × 122 px · 4 สี']];
    const cols = 3, gx = 0.35, gy = 0.35, cw = (W - 2 * M - gx * 2) / cols, ch = 2.35;
    for (let i = 0; i < K.length; i++) {
      const x = M + (i % cols) * (cw + gx), y = 1.65 + Math.floor(i / cols) * (ch + gy);
      card(s, x, y, cw, ch);
      await badge(s, K[i][0], x + 0.35, y + 0.35, 0.55);
      txt(s, K[i][1], {x: x + 0.35, y: y + 1.0, w: cw - 0.6, h: 0.75, bold: true, color: C.text2, fontSize: i === 4 ? 32 : 40});
      txt(s, K[i][2], {x: x + 0.35, y: y + 1.75, w: cw - 0.6, h: 0.45, color: C.accent6, fontSize: 14});
    }
    s.addNotes('7 วันเป็นค่าเป้าหมายจากแบต 1500 mAh ยังไม่ได้วัดบนเครื่องต้นแบบ');
  }
  const table = (s, x, y, w, head, rows, ic) => {
    const opts = {color: C.text1, fontSize: 14, valign: 'middle', margin: [0.06, 0.12, 0.06, 0.12]};
    const data = [[{text: head, options: {colspan: 2, bold: true, fontSize: 15, color: C.background1, fill: {color: C.accent1}}}],
      ...rows.map(([k, v]) => [{text: k, options: {bold: true, color: C.accent6}}, {text: v}])];
    s.addTable(data, {x, y, w, colW: [w * 0.34, w * 0.66], ...opts, border: {type: 'solid', pt: 0.75, color: 'E3EAEE'}, fill: {color: 'FFFFFF'}, rowH: 0.42});
  };
  const TGT = ' (เป้าหมาย)';
  {
    const s = content('03 · SPECIFICATION', 'ขนาดตัวเครื่อง');
    pic(s, 'device_dimensions_light', M, 1.5, 6.9, 5.25);
    table(s, 7.85, 1.75, W - M - 7.85, 'ตัวเครื่อง', [
      ['ขนาด', '87 × 87 × 12.3 มม.'], ['จอแสดงผล', 'e-Paper 2.13″ · 250 × 122 px · 4 สี'], ['ไฟสถานะ', 'RGB 3 ดวง + ไฟชาร์จด้านข้าง'],
      ['เสียงเตือน', 'Buzzer'], ['สวิตช์', 'เปิด/ปิด ตัดไฟทั้งเครื่อง'], ['พอร์ต', 'USB-C (ชาร์จ · ไดรฟ์ USB)'], ['ประมวลผล', 'ESP32-S3 · PSRAM 8 MB']]);
    s.addNotes('ขนาดวัดจากแบบ 3D ของเคส (ไฟล์ STEP)');
  }
  {
    const s = content('03 · SPECIFICATION', 'ข้อมูลจำเพาะ · การวัดและการติดตาม');
    const cw = (W - 2 * M - 0.4) / 2;
    table(s, M, 1.65, cw, 'การวัดอุณหภูมิ', [
      ['หัววัด', 'Thermocouple Type K ปลอกสแตนเลส'], ['ตัวแปลงสัญญาณ', 'MAX6675 · ความละเอียด 0.25 °C'], ['ช่วงวัด', '0 °C ขึ้นไป · เหมาะกับ 2–8 °C'],
      ['รอบบันทึก', 'ทุก 5 นาที (ตั้งได้ 5–60 นาที)'], ['Alarm', 'นอกช่วงต่อเนื่อง 5 นาที (ตั้งได้)'], ['Alarm หาย', 'กลับเข้าช่วงลึก 0.5 °C']]);
    table(s, M + cw + 0.4, 1.65, cw, 'การติดตาม', [
      ['ตำแหน่ง', 'GNSS ATGM336H'], ['ฝากล่อง', 'Reed switch บันทึกการเปิด-ปิด'], ['การขยับ', 'LIS2DW12 นับการขยับและแรงกระแทก'], ['นาฬิกา', 'RTC เดินต่อแม้ปิดเครื่อง']]);
    txt(s, 'ความละเอียด 0.25 °C ไม่ใช่ความแม่นยำ ความแม่นยำขึ้นกับผลสอบเทียบ · MAX6675 อ่านค่าติดลบไม่ได้',
      {x: M, y: 6.45, w: W - 2 * M - 1.6, h: 0.35, fontSize: 11, color: C.accent6, italic: true});
    s.addNotes('ตัวเลขทั้งหมดมาจากเอกสาร requirement คู่มือผู้ใช้ และเอกสารค่าตั้งค่าของแอปใน repo');
  }
  {
    const s = content('03 · SPECIFICATION', 'ข้อมูลจำเพาะ · การเชื่อมต่อ ข้อมูล และพลังงาน');
    const cw = (W - 2 * M - 0.4) / 2;
    table(s, M, 1.65, cw, 'การเชื่อมต่อ', [
      ['Wi-Fi', '2.4 GHz · จำได้ 5 เครือข่าย'], ['ส่งข้อมูล', 'MQTT ทุกครั้งที่บันทึก'], ['Bluetooth', 'BLE เปิด 60 วินาทีหลังแตะ NFC'],
      ['NFC', 'ST25DV04KC · แตะเพื่อเชื่อมต่อ'], ['USB-C', 'ชาร์จ · ไดรฟ์ USB ส่งออก CSV']]);
    table(s, M + cw + 0.4, 1.65, cw, 'ข้อมูลและพลังงาน', [
      ['หน่วยความจำ', 'Flash 16 MB · ประมาณ 268 วัน'], ['การ์ดเสริม', 'microSD (optional)'], ['แบตเตอรี่', 'Li-Po 1500 mAh' + TGT],
      ['ใช้งานต่อเนื่อง', '7 วัน บันทึกทุก 5 นาที' + TGT], ['การชาร์จ', 'USB-C PD · Dock ชาร์จ 6 ช่อง'], ['ป้องกันแบต', 'ต่ำกว่า 5% ปิดเครื่องเอง']]);
    txt(s, 'ค่าที่ระบุ "เป้าหมาย" เป็นค่าออกแบบ ยังไม่ได้วัดจริงบนเครื่องต้นแบบ',
      {x: M, y: 6.45, w: W - 2 * M - 1.6, h: 0.35, fontSize: 11, color: C.accent6, italic: true});
    s.addNotes('แบตต่ำกว่า 10% เครื่องไม่ให้เริ่มทริปใหม่ ต่ำกว่า 5% จอขึ้น BATTERY EMPTY แล้วปิดเครื่องเองเพื่อถนอมแบต ข้อมูลไม่หาย');
  }
  {
    const s = content('03 · SPECIFICATION', 'ข้อมูลจำเพาะ · TCOLD Dock');
    pic(s, 'dock_dimensions_light', M, 1.55, 6.6, 3.0);
    pic(s, 'dock_top', M + 1.2, 4.6, 4.2, 2.2);
    table(s, 7.55, 1.65, W - M - 7.55, 'TCOLD Dock', [
      ['จำนวนช่อง', '6 ช่อง ชาร์จพร้อมกัน'], ['ขนาด', '164.5 × 119.4 × 59.4 มม.'], ['ไฟเข้า', 'USB-C PD 45 W' + TGT],
      ['ไฟแต่ละช่อง', '5 V · ตัดกระแสเกินแยกช่อง'], ['ต่อคอมพิวเตอร์', 'USB-C สายเดียว ผ่าน hub ในตัว'],
      ['การป้องกัน', 'TVS + eFuse ขาเข้า · ESD'], ['สถานะชาร์จ', 'ไฟข้างของแต่ละเครื่อง'], ['ตัว Dock', 'ไม่มี MCU ไม่มี firmware']]);
    txt(s, 'ต้องเสียบ adapter PD เสมอ ไฟจากคอมพิวเตอร์อย่างเดียวไม่พอ · ขนาดวัดจากแบบ 3D (Dock Assy.STEP)',
      {x: M, y: 6.45, w: W - 2 * M - 1.6, h: 0.35, fontSize: 11, color: C.accent6, italic: true});
    s.addNotes('ตัวเลข Dock มาจากตาราง hardware ใน README และหัวข้อ 6 ของเอกสาร requirement ขนาดวัดจากไฟล์ 3D');
  }
  // closing
  {
    const s = pres.addSlide({masterName: 'COVER'});
    s.addText('THANES · SMART COLD CHAIN', {placeholder: 'kicker'});
    s.addImage({path: obj('TCOLD_lockup_on_dark'), x: M, y: 1.65, w: 5.6, h: 5.6 * 540 / 2331, altText: 'TCOLD'});
    s.addText('ขอบคุณ', {placeholder: 'title'});
    s.addText('Smart Cold Chain · Smart Lab · Smart Pharmacy · Smart AI', {placeholder: 'body'});
    s.addShape(pres.shapes.ROUNDED_RECTANGLE, {x: 7.25, y: 0.75, w: 5.5, h: 6.0, rectRadius: 0.3, fill: {color: 'F2F8FA'}, line: {type: 'none'}});
    pic(s, 'device_front', 7.6, 1.1, 4.8, 5.3);
  }

  await pres.writeFile({fileName: OUT});
  await writeTheme(OUT, THEME);
  console.log('wrote', OUT);
})();
