# epd29-s3 — bench test สำหรับจอ e-paper 2.9"

ทดสอบด้วย **ESP32-S3 devkit + จอ 2.9" ที่ไม่ใช่ของโปรเจกต์นี้** เพื่อดู layout บนหมึกจริง
วัดเวลา refresh จริง และไล่หาว่าโมดูลตรงกับ panel class ไหน

## เปิดใน VS Code

ติดตั้ง extension **PlatformIO IDE** → `File > Open Folder` เลือกโฟลเดอร์ `bench/epd29-s3`
แล้วกดปุ่มล่างซ้าย: `✓` build · `→` upload · `🔌` serial monitor
(หรือ `pio run -t upload -t monitor` ใน terminal)

PlatformIO จะโหลด toolchain กับ library ให้เองครั้งแรก ใช้เวลาสักพัก

## การต่อสาย

| ESP32-S3 | จอ | หมายเหตุ |
|---|---|---|
| GPIO39 | SCL / SCK | |
| GPIO40 | SDA / MOSI | จอเป็น write-only ไม่ต้องต่อ MISO |
| GPIO41 | DC | |
| GPIO42 | CS | |
| GPIO38 | BUSY | |
| **EN** | RES | ต่อกับขา EN ของบอร์ด |
| 3V3 / GND | VCC / GND | |

**RES ต่อกับ EN แปลว่า firmware สั่ง reset จอเองไม่ได้** จอจะ reset พร้อม MCU เท่านั้น
โค้ดจึงส่ง `PIN_RST = -1` เข้า driver ซึ่งตรวจสอบแล้วว่า `GxEPD2_EPD::_reset()`
ข้ามการ reset เมื่อค่าติดลบ (ไม่ได้ไป toggle ขามั่ว) แล้วไปใช้ software reset ใน init sequence แทน
ถ้าจอค้าง ให้กดปุ่ม reset บนบอร์ด — EN ลง LOW จะ hard reset ทั้ง MCU และจอพร้อมกัน

## เลือก panel class ให้ตรงรุ่น

สาเหตุอันดับหนึ่งที่ "ต่อถูกแล้วแต่จอไม่ขึ้น" คือ class ไม่ตรงรุ่น แก้ที่หัวไฟล์ `src/main.cpp`

| Class | โมดูล | full refresh |
|---|---|---|
| `GxEPD2_290_T94_V2` | GDEM029T94 / Waveshare 2.9" V2 (SSD1680) | 4.1 s |
| `GxEPD2_290_T94` | GDEM029T94 V1 | |
| `GxEPD2_290` | GDEH029A1 (SSD1608 รุ่นเก่า) | |
| `GxEPD2_290_BS` | DEPG0290BS | |
| `GxEPD2_290_C90c` | GDEM029C90 ขาวดำแดง | 27 s |
| `GxEPD2_290_Z13c` | GDEH029Z13 ขาวดำแดง | 18 s |

ถ้าเป็นรุ่น 3 สี ตั้ง `#define USE_3C 1` ด้วย

## สิ่งที่โค้ดทำ

1. อ่านระดับขา BUSY **ก่อน** เริ่ม SPI แล้วพิมพ์ออก serial — สายหลุดหรือขาลอย
   จะดูเหมือนจอเสียทุกประการถ้าไม่เช็กตรงนี้ก่อน
2. ย้าย SPI มาที่ขาของบอร์ดนี้ (`SPI.begin(39, -1, 40, 42)`)
3. วาด 3 หน้า วนไปเรื่อย ๆ: ink test (ดำ/แดง/เส้นถี่), หน้าปกติ, หน้า alarm
   ใช้ layout เดียวกับ `docs/display-design.md` — header กลับสี, อุณหภูมิกึ่งกลาง,
   min/max ล่างสุด, เส้นคั่น, แถวสถานะ, alarm เป็นกรอบ
4. **จับเวลา refresh ทุกครั้งแล้วพิมพ์ออก serial**

## ข้อควรรู้ก่อนเอาตัวเลขไปใช้

- จอ 2.9" ขาวดำ refresh ~4 s ส่วนจอจริงของ mCOLD เป็น 4 สี (G) ~16–25 s
  **ถ้าอยากรู้สึกถึงความหน่วงจริง ให้ทดสอบกับรุ่น 3 สี** (18–27 s) ซึ่งใกล้เคียงกว่ามาก
  ตัวเลขที่วัดจากจอขาวดำใช้ยืนยัน refresh policy ใน `docs/display-design.md` ไม่ได้
- จอนี้ 296×128 ส่วนของจริง 250×122 — layout จึงเป็นภาพใกล้เคียง ไม่ใช่ pixel ต่อ pixel
  ถ้าต้องการ pixel-accurate ให้ดู PNG ใน `display-mock/out/`
- ใช้ฟอนต์ FreeSans ของ Adafruit GFX ไม่ใช่ IBM Plex ที่ออกแบบไว้ น้ำหนักเส้นจะต่างจากแบบ
- โค้ดนี้ **ยังไม่ได้ compile** เพราะ network policy ของ environment ที่เขียนบล็อก
  registry ของ PlatformIO · ตรวจ API กับ source จริงของ GxEPD2 แล้ว
  (constructor, `HEIGHT/WIDTH`, `hasFastPartialUpdate`, 4-argument `init()`,
  พฤติกรรมของ `_reset()` เมื่อ rst ติดลบ) แต่ยังไม่ได้ build จริง

## ถ้าจอไม่ขึ้น

| อาการ | ตรวจ |
|---|---|
| serial ไม่มีอะไรเลย | เสียบพอร์ต USB ผิดตัว — บอร์ด S3 มีสองพอร์ต · ถ้าใช้พอร์ต UART ให้เปลี่ยนเป็น `-DARDUINO_USB_CDC_ON_BOOT=0` ใน `platformio.ini` |
| BUSY อ่านได้ค่าเดิมตลอด | สาย BUSY ไม่ได้ต่อ หรือต่อผิดขา |
| ค้างที่ `init()` | driver รอ BUSY ไม่จบ — สาย BUSY หรือ panel class ผิด |
| จอขาวล้วน ไม่มีอะไร | panel class ผิดรุ่น ลองไล่ตามตารางด้านบน |
| ภาพเพี้ยน/เลื่อน | class ใกล้เคียงแต่ไม่ตรง หรือ rotation ไม่ตรง (`setRotation`) |
