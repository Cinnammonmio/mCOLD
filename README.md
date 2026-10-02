# mCOLD

อุปกรณ์ติดตามกล่องโฟมขนส่งยาและเวชภัณฑ์ (cold-chain box tracker) ประกอบด้วยตัวเครื่องติดกล่อง
และแท่นชาร์จ 6 ช่อง อยู่ระหว่างพัฒนาต้นแบบ

Firmware ของตัวเครื่องอยู่ที่ [`firmware/`](firmware/) (ESP-IDF) · เวอร์ชันแยกด้วย tag `firmware/vX.Y.Z`
· ประวัติแต่ละเวอร์ชันอยู่ใน [`firmware/CHANGELOG.md`](firmware/CHANGELOG.md)
· สถานะงานและข้อควรรู้อยู่ใน [`firmware/HANDOFF.md`](firmware/HANDOFF.md)

> โปรเจกต์นี้เดิมชื่อ **Foam** เปลี่ยนเป็น **mCOLD** เมื่อ 2026-09-20
> ไฟล์ schematic และเอกสาร requirement ยังใช้คำว่า `foam` ภายใน เพราะเป็นชื่อไฟล์/ชื่อ net จริงบนบอร์ดที่ผลิตไปแล้ว

## ฮาร์ดแวร์

| ส่วน | รายละเอียด |
|---|---|
| MCU | ESP32-S3-WROOM-1U-N16R8 (Flash 16 MB, PSRAM 8 MB octal) |
| อุณหภูมิ | MAX6675 + thermocouple Type K |
| เซ็นเซอร์ | LIS2DW12 (accel), reed switch (ประตู), ATGM336H (GNSS) |
| จอ | Waveshare 2.13" e-Paper (G) 250×122 4 สี — **V2** |
| พลังงาน | BQ25601 charger, HUSB238A PD sink, TPS63020, MAX17048, INA226 |
| อื่น ๆ | ST25DV04KC (NFC), PCF8523 (RTC), microSD (SPI, optional), buzzer MLT-8530 |
| Dock | 6 ช่อง ไม่มี MCU · FE1.1S hub ×2 · SY6280 ×6 · ต้องใช้ adapter USB-C PD 45 W |

## โครงสร้าง repo

```
docs/
  mCOLD_Firmware_Requirements_2026-09-19.md   เอกสาร requirement ฉบับเต็ม (แหล่งอ้างอิงหลัก)
  display-design.md                           สเปกหน้าจอ e-paper (layout, type, ไอคอน, refresh)
  led-design.md                               สเปกไฟ RGB 4 ดวง (หน้าที่แต่ละดวง, pattern, event)
  SCH_Schematic_foamV1.0.1_P.1_2026-09-19.pdf schematic ตัวเครื่อง
  SCH_Schematic_foam_dock_P.3_2026-09-19.pdf  schematic dock
display-mock/                                 renderer + ภาพหน้าจอทุก state ขนาดจริง
led-mock/                                     renderer + ภาพ pattern ไฟและผังตำแหน่ง
mcold-spec/                                   ต้นฉบับเอกสารสเปกสำหรับผู้บริหาร/ฝ่ายขาย
mCOLD_Functions_and_Usage_Flow_2026-09-20.pdf เอกสารที่ build ออกมาแล้ว (8 หน้า)
```

## สถานะฮาร์ดแวร์

PCB สั่งผลิตแล้วแต่ยังไม่ได้รับของ การแก้ไขใด ๆ ต้องทำแบบ rework หน้างาน

ผลตรวจ schematic เทียบกับเอกสาร requirement — **GPIO map ในเอกสารถูกต้องทุกขา** รวมถึง
GNSS (GPIO17 = MCU TX → GNSS RXD0, GPIO18 = MCU RX), LED chain (LED4 → LED1 → LED2 → LED3),
CHG_CE_N pull-down R22 10 kΩ, INA226 VIN+ = BAT_CHARGER (กระแสเป็นบวกตอนชาร์จ)

### ประเด็นที่ต้องรู้ก่อนเขียน firmware

1. **Dock: net `5V_SYS` ไม่ได้ต่อโหลดใดเลย** — TPS2116 power mux ไม่ทำงาน, hub และ SY6280 ทุกตัว
   รับไฟจาก `5V_POWER` (buck จาก 20 V PD) → **Dock ต้องเสียบ PD adapter เสมอ** ใช้ PC อย่างเดียวไม่ได้
   สรุปว่าไม่ rework เพราะพอร์ต PC จ่ายไฟไม่พอสำหรับ 6 เครื่องอยู่แล้ว
2. **RTC `VBAT_3V` เป็น net ลอยตามแบบ** ไม่มี backup → ปิดสวิตช์แล้วเวลาหาย · **บอร์ด prototype ตอนนี้บัดกรี CR1220 เข้า VBAT_3V แล้ว (2026-10-02)** และ firmware เปิด battery switch-over ให้ → เวลาไม่หายตอนปิดเครื่อง · รุ่นถัดไปควรใส่ถ่าน/supercap ไว้ในแบบ
   (rework เชื่อม VBAT ลง VSS ใช้เฉพาะบอร์ดที่ไม่มีถ่านเลย **ห้ามทำกับบอร์ดที่ใส่ถ่านแล้ว**)
3. **HUSB238A VDD มาจาก 3V3_MAIN** → SW3 OFF แล้วชิป PD ไม่มีไฟ อาจไม่ present Rd บน CC
   ต้องทดสอบจริงว่าเสียบที่ชาร์จ C-to-C ตอนปิดสวิตช์แล้วมี VBUS หรือไม่
   (ชาร์จใน Dock ตอน SW3 OFF ใช้ได้แน่นอน เพราะ Dock เปิด VBUS ทุกช่องตลอดเวลา)
4. **pull-up ของ CS อยู่บน switched rail** (SD_CS → 3V3_SD_GNSS, MAX6675_CS/EPD_CS/EPD_RST → 3V3_EPD_TK)
   → ตอน rail ดับต้องตั้งขาเป็น input/Hi-Z ห้าม drive HIGH ค้าง มิฉะนั้น back-power
5. **SD card-detect ต่อ GND** ไม่ได้ต่อ MCU → ตรวจการ์ดด้วยการลอง init SPI เท่านั้น
6. **วงจร boost ของ e-paper ต่างจาก reference ของ Waveshare**: L7 68 µH (ref 47 µH),
   R33 470 mΩ (ref 2.2 Ω), R44 10 kΩ (ref 1 MΩ) → เปิดครั้งแรกต้องจำกัดกระแสและวัด PREVGH/PREVGL ก่อน
7. **ไม่มี connector ของ door / thermocouple / NFC antenna ใน schematic** — ยืนยันแล้วว่ามี pad บน PCB

### เรื่องที่ปิดประเด็นแล้ว

- **จอ**: SKU 24797 มี full refresh 16 s / fast 11 s ตรงกับเอกสาร V2 และ Waveshare ระบุว่า
  V2 ใช้ driver demo ของ V1 ได้ (V2 เพิ่มแค่ fast refresh) → ใช้ init sequence V1 เป็น baseline,
  fast refresh เป็น feature flag · SPI mode 0 · โค้ดอ้างอิง https://github.com/waveshare/e-Paper
- **Buzzer**: LCSC C94599 = MLT-8530 เป็น **passive electromagnetic** ต้องขับ PWM 2.7 kHz
  ขดลวด 16 Ω → peak ~200 mA ที่ 3.3 V ดังนั้น beep ต้องสั้น (50–200 ms) · Q5 = MMBT2222A,
  R34 = 1 kΩ ให้ Ib แค่ ~2.6 mA จะไม่ saturate เต็ม ถ้าเสียงเบาให้เปลี่ยน R34 เป็น 330 Ω

## หน้าจอ

ออกแบบครบ 14 state แล้ว ดูภาพขนาดจริงได้ที่ `display-mock/out/contact_sheet.png`
รายละเอียด layout/type/ไอคอน/refresh policy อยู่ที่ `docs/display-design.md`
สร้างภาพใหม่ด้วย `python display-mock/render.py`

ใช้ 3 หมึก (ขาว/ดำ/แดง) · header กลับสี · อุณหภูมิกึ่งกลางใหญ่สุด min/max ล่างสุด ·
footer แถวสถานะ (wifi, cloud, trip, shock, charge, แบต, เมม) · alarm เป็นกรอบแดง
โค้ด layout ยกไปเป็น view model ใน firmware ได้ตรง ๆ เหลือเปลี่ยน backend เป็น panel driver

## ไฟ LED

4 พิกเซล RGB: สามดวงหน้าเครื่อง (สินค้า / ทำงานปกติ / เครื่องมีปัญหา) และหนึ่งดวงข้างเครื่องบอกการชาร์จ
รายละเอียดอยู่ที่ `docs/led-design.md` ภาพ pattern อยู่ที่ `led-mock/out/`

กฎสำคัญ: **เปิด rail เฉพาะตอนกระพริบ** เพราะชิป addressable กินไฟแม้ดับ (~4 mA รวมสี่ดวง)
ถ้าเปิดค้างจะกินเกินครึ่งของแบตในหนึ่งสัปดาห์ · ส่วนตัวกระพริบเองแทบไม่มีต้นทุน

## แผนพัฒนา firmware

Hardware พิสูจน์แล้วด้วย [`bringup/`](bringup/) · product firmware อยู่ที่ [`firmware/`](firmware/)
แต่ละเฟสจบด้วย release `firmware/v0.N.0` ที่ทดสอบบนบอร์ดจริงแล้ว
รายละเอียดเต็มอยู่ใน [`firmware/HANDOFF.md`](firmware/HANDOFF.md) §7

| เฟส | Release | เนื้อหา | สถานะ |
|---|---|---|---|
| P0 โครงระบบ | 0.0.1 | ESP-IDF, partition, health registry, rail, bus, task + supervisor | ✅ |
| P1 drivers | 0.1.0 | อุปกรณ์บนบอร์ดครบทุกตัว · ไฟข้างเครื่องบอกสถานะชาร์จ | ✅ (GNSS ยังไม่เคย fix) |
| P2 เวลา/ตั้งค่า/flash log | 0.2.0 | UTC + ความน่าเชื่อถือ · config ใน NVS · log ทนไฟดับ | ✅ |
| P3 trip + sensing | 0.3.0 | เริ่ม/จบ trip พร้อม alarm ของ trip นั้น · เก็บข้อมูลทุก 5 นาที · event · alarm · กู้ trip หลัง reset · ลบ trip เก่าเมื่อเต็ม · ปิดฟังก์ชันประตู | ✅ |
| P4 จอ, LED, buzzer | 0.4.0 | จอ e-paper (ใช้จอขาวดำไปก่อน) · pattern ไฟและเสียงตาม `docs/led-design.md` | ✅ · จอ 4 สียังไม่มา |
| P5 BLE/NFC | 0.5.0 | BLE GATT · ชุดคำสั่ง · NDEF · แตะ NFC เพื่อยืนยันสิทธิ์ (ไม่ต้อง pair) | ✅ · รอทีม iOS ยืนยัน PROTOCOL.md |
| P6 sync, USB, OTA | 0.6.0 | Wi-Fi + MQTT · ACK แล้วคืนพื้นที่ · USB drive + CSV · SD · OTA | กำลังทำ · รอ Wi-Fi, topic/ACK/TLS จากทีม server |
| P7 พลังงาน | 0.7.0 | deep sleep · wake ทุกแหล่ง · charger/PD · วัดจริงให้ได้ 7 วัน | รอ datasheet แบต |
| P8 พร้อมผลิต | 1.0.0 | watchdog · security · acceptance test §14 · Dock 6 เครื่อง · factory provisioning | — |

## เรื่องที่ยังรอ

- ผลทดสอบ HUSB238A ตอน SW3 OFF กับที่ชาร์จ C-to-C
- register map ฉบับเต็มของ HUSB238A-BB001 (ยังมีแค่ product brief) → PD 9 V ปิดไว้ก่อน
- รูปแบบ Server API / ACK contract
- BLE UUID และ NDEF schema ให้ตรงกับแอป iOS
- ผลสอบเทียบอุณหภูมิ และผลวัดกระแสจริงเพื่อยืนยันเป้าหมาย 7 วัน / 31 วัน

## การ build เอกสารสเปก

```bash
python mcold-spec/build.py
```

แก้ข้อความที่ `mcold-spec/template.html` แล้วรันคำสั่งข้างบน จะได้ PDF ใหม่ที่ root ของ repo
ต้องมี Python + pymupdf และ Microsoft Edge (ใช้ headless print) · ฟอนต์และไอคอนอยู่ใน repo แล้ว
ถ้าต้องโหลดใหม่ใช้ `python mcold-spec/fetch_assets.py`
