# mockup

ภาพนำเสนอสินค้า **TCOLD** (ชื่อเดิมของโปรเจกต์คือ mCOLD) เรนเดอร์จากโมเดล 3D จริงของเคส
(`Assy CASE_80CM_V2x.STEP` ที่อยู่ใน `../Assy CASE_80CM_V2x.rar`)

## ชื่อและโลโก้

**TCOLD — Thanes Cold-chain Online Logging Device**
อักษรแรกของแต่ละคำประกอบเป็น T·C·O·L·D และอยู่ในสาย *Smart Cold Chain* ของ THANES

- ตัว T เอามาจากโลโก้ THANES (วัดจากภาพถ่ายแล้วสร้างใหม่เป็น vector): T ที่ถูกตัดด้วยเส้นทแยง
  45° เส้นเดียวผ่านแขนซ้ายและหัวก้าน มุมที่เส้นตัดชนขอบมีความโค้ง ครึ่งขวาของคานไม่ถูกตัด
- COLD ใช้ฟอนต์ Unbounded Bold (OFL) แปลงเป็นเส้นแล้ว ไฟล์ SVG จึงไม่ต้องติดตั้งฟอนต์
  เลือก Bold เพราะความหนาเส้นใกล้ T มากที่สุด
- ชื่อเต็มใช้ IBM Plex Sans ใกล้กับแถว "Smart Cold Chain · Smart Lab …" ในโลโก้ THANES
- ไฟล์อยู่ใน `brand/` สร้างใหม่ด้วย `python mockup/brand/logo.py`

| ไฟล์ | ใช้กับ |
|---|---|
| `TCOLD_logo.svg` | พื้นสว่าง (T teal ไล่สี, COLD navy) |
| `TCOLD_logo_on_dark.svg` | พื้นเข้ม (T teal, COLD ขาว) |
| `TCOLD_logo_white.svg` / `TCOLD_logo_navy.svg` | สีเดียว |
| `TCOLD_lockup.svg` / `TCOLD_lockup_on_dark.svg` | โลโก้ + ชื่อเต็ม |

## ภาพสไลด์สำเร็จ

| ไฟล์ | หน้า | เนื้อหา |
|---|---|---|
| `out/TCOLD_mockup.png` | `hero.html` | ตัวเครื่อง + thermocouple + แอปบนมือถือ |
| `out/TCOLD_explode.png` | `explode.html` | exploded view 9 ชิ้น พร้อมภาพบอร์ดหลักด้านอุปกรณ์ |

![TCOLD mock-up](out/TCOLD_mockup.png)
![TCOLD exploded view](out/TCOLD_explode.png)

ทั้งสองภาพขนาด 3840×2160 (16:9)

## ชิ้นงานแยก (PNG พื้นใส) สำหรับวางบนธีมสไลด์

`out/objects/` ตัดขอบให้พอดีชิ้นแล้ว เว้นขอบ 24 px ไม่มีพื้นหลัง ไม่มีป้ายชี้
ใส่ป้ายและเงาด้วยธีมของสไลด์เอง

| ไฟล์ | เนื้อหา |
|---|---|
| `TCOLD_logo*.png`, `TCOLD_lockup*.png` | โลโก้กว้าง 2400 px |
| `device_with_probe.png` | ตัวเครื่องมุมเดียวกับสไลด์หลัก พร้อมสายและหัววัด |
| `device_front.png` / `device_back.png` | ตัวเครื่องตรงหน้า / ด้านหลัง |
| `phone_app.png` / `phone_app_tilted.png` | มือถือพร้อมหน้าจอแอป ตรง / เอียง |
| `explode_stack.png` | ชุด explode ทั้งชุด ไม่มีป้าย |
| `part_1_front_case.png` … `part_8b_back_case_outside.png` | แต่ละชิ้นในมุมเดียวกับ explode ใช้เรียงเองในสไลด์ได้ (6b = บอร์ดหลักด้านอุปกรณ์, 8b = ฝาหลังด้านนอก) |
| `thermocouple.png` | สายและหัววัด |
| `epaper_screen_trip.png` | ภาพหน้าจอ e-paper TRIP ACTIVE |

## หมายเหตุ

- จอ e-paper คือ state A2 TRIP ACTIVE วาดด้วย `../display-mock/render.py` โดยเปลี่ยนรหัสเครื่องเป็น TCOLD-0117
  มีกรอบเลนส์สีเข้มรอบช่องจอ (กรอบนี้เป็นสีสำหรับนำเสนอ ไม่มีในไฟล์ STEP)
- ไฟหน้า 3 ดวงติดทุกดวงเพื่อให้เห็นสีประจำตำแหน่งตาม `../docs/led-design.md`:
  ซ้าย CARGO แดง · กลาง TRIP ฟ้า · ขวา DEVICE เหลือง และพิมพ์ชื่อไว้ใต้ไฟ
  ตอนใช้งานจริงไฟติดเป็นจังหวะตามเหตุการณ์ ไม่ได้ติดพร้อมกัน
- หน้าจอแอปในมือถือเป็นภาพแนวคิด (`phone.js`) ไม่ใช่ภาพจากเดโมแอป
  ตัวเลขทุกตัวตรงกับหน้าจอ e-paper ส่วนช่วง 2–8 °C และปุ่มต่าง ๆ อิงคู่มือ
  `../docs/manuals/user-manual-th.md`
- thermocouple จำลองจากหัววัดจริง (สายถักลายฟ้า ปลอกสแตนเลส ข้อความ laser
  `T/C K(S) 75309005 /08-26`)
- ใน STEP บอร์ดหลักเป็นพื้นผิว 2,148 ชิ้นที่ไม่มีข้อมูลสี `prepare.py` จึงจัดกลุ่มตามขนาด
  (บอร์ด, shield โลหะ, ขา/pad, passive, ชิปดำ) เพื่อลงสี ป้ายชิ้นส่วนในภาพบอร์ด
  ชี้ตามขนาด footprint ที่ตรงกับ datasheet (ESP32-S3-WROOM-1U 18 × 19.2, ATGM336H 10.1 × 9.7)
- สีเคส (หน้าขาว หลัง teal) และโลโก้บนหน้าเครื่องเป็นสีสำหรับนำเสนอ ไม่ใช่สีผลิตจริง
- สีและฟอนต์ใช้ชุดเดียวกับ `../mcold-spec/template.html`

## สร้างภาพใหม่

```bash
pip install cadquery-ocp trimesh pillow scipy rtree
bsdtar -xf "Assy CASE_80CM_V2x.rar" -C /tmp          # หรือแตกด้วย WinRAR / 7-Zip
python mockup/prepare.py "/tmp/Assy CASE_80CM_V2x.STEP" # -> mockup/build/ (STL รายชิ้น + texture จอ)
cd mockup && npm install
python brand/logo.py                                     # โลโก้ SVG
node render.js '[["hero.html","out/TCOLD_mockup.png",1920,1080,2],["explode.html","out/TCOLD_explode.png",1920,1080,2]]'
python objects.py                                         # ชิ้นงานแยก -> out/objects/
```

`render.js` ใช้ Playwright + Chromium (WebGL ผ่าน SwiftShader) เปิดหน้าแล้ว screenshot ที่ 2×
ส่วนที่ทั้งสองหน้าใช้ร่วมกัน (โหลดชิ้นงาน, วัสดุ, จอ, กรอบ, ไฟ, thermocouple, ป้ายชี้) อยู่ใน `scene.js`

ปรับได้ผ่าน query string โดยไม่ต้องแก้โค้ด เช่น
- `hero.html?cam=[60,135,520,23,-40,12]&ph=[1330,170]&pr=4.71`: มุมกล้อง, ตำแหน่งมือถือ, การหมุนหัววัด
- `explode.html?dz={"top":0,"battery":40,...}&rot=[-0.1,-0.85,0]&lab={"5":[1000,170,"r"]}`:
  ระยะแยกชิ้น, มุมหมุนทั้งชุด, ตำแหน่งป้ายชิ้นที่ 1–9

พิกัดที่วัดจาก STEP (หน่วย mm ตามแกนของไฟล์):
เคส 87 × 87 × 12 · หน้าเครื่องคือ +Z (ชิ้น `Main Case_BOT`) ·
ช่องจอ x −45.5…2.5, y −5.7…17.3 (48 × 23 ตรงกับ active area ของจอ 2.13″) ·
light pipe หน้า 3 ดวงที่ y −18.2 · ไฟข้างอยู่ขอบบน · บอร์ดหลักหนา 1.6 อุปกรณ์อยู่ด้านหลัง
และมีช่องเจาะให้แบต
