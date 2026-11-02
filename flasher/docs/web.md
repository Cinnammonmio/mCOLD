# mCOLD Line — เว็บสายผลิต (สำหรับทีม server)

เว็บนี้แทนหน้าต่าง Dock เดิม: หน้าหลักคือ **รายการเครื่องของล็อต** มีปุ่ม **เพิ่มเครื่อง**
เสียบสาย USB ที่กล่อง กดเพิ่ม เบราว์เซอร์ flash firmware และตั้งค่าในครั้งเดียว ถอดสายวางบนที่ชาร์จ
จากนั้นทดสอบทุกอย่างผ่าน Wi-Fi → MQTT คนยืนยันสิ่งที่ต้องดูด้วยตา แล้ว OTA เป็น firmware จริง

```
เพิ่มเครื่อง (USB, ในเบราว์เซอร์)              ทดสอบ (MQTT, หลายเครื่องพร้อมกัน)
 เลือกพอร์ต → อ่าน MAC                         ต่อ broker, IP ตรงเลขเครื่อง, firmware ตรง
 server จอง SN + เตรียมไฟล์                     status: อุณหภูมิ, ไฟเลี้ยง
 ลบชิป → เขียน bootloader, partitions,          DIAG_RUN: I2C, อุณหภูมิ, GNSS, accel, NFC, ชาร์จ, ราง, logtest
   otadata, firmware และ NVS (SN, Wi-Fi,         คนยืนยัน: กล่องไหนกะพริบ, ไฟทุกสี, เสียง, จอ
   broker, OTA base, ค่าตั้ง, factory mode)      ดึง debug → ส่งค่าตั้ง → OTA firmware จริง
 รีสตาร์ท → บันทึก (ขั้น usb)                    ส่งมอบ Wi-Fi (ข้อสุดท้าย) → บันทึก (ขั้น line)
```

## ใช้งาน

1. ตั้งค่า (⚙): Wi-Fi โรงงาน + IP pool, Wi-Fi ลูกค้า, MQTT broker, debug broker, OTA base, ค่าตั้งต้น
2. ⋯ → Firmware ที่ใช้: นำเข้า zip ของ build (`.pio/build/mcold`) อนุมัติ แล้วเลือก
   - **Flash ผ่าน USB**: รุ่นที่เขียนตอนเพิ่มเครื่อง
   - **Firmware จริง**: รุ่นที่ OTA เป็นข้อท้าย ๆ ของการทดสอบ (ถ้าเหมือนกันก็ข้าม)
3. กรอกล็อต → **เพิ่มเครื่อง** → เสียบสาย → **Enter** → เลือกพอร์ต USB JTAG → รอเขียว → ถอดสาย วางบนที่ชาร์จ → ตัวถัดไป
   เลขเครื่องเลื่อนให้เอง ค่าที่เลือกจำไว้
4. **ทดสอบเครื่องที่พร้อม** (หรือเปิด "ทดสอบเองเมื่อ online") → ตอบคำถามที่ขึ้นทีละเครื่อง ด้วย Y / N / R
5. เครื่องที่ **ผ่าน** ออกจาก Wi-Fi โรงงานแล้ว ถอดเก็บได้ · **ไม่ผ่าน** ยังอยู่ที่ IP เดิม ส่งซ่อม (กด debug ดู stream ได้)
6. **สรุป SN** + CSV

## สิ่งที่ต้องมี

- **Chrome หรือ Edge** บน PC ที่เสียบสาย (Web Serial) และเปิดเว็บผ่าน **https** (หรือ localhost)
  Firefox/Safari ต่อ USB ไม่ได้
- กล่องใหม่ขึ้นเป็น USB-Serial-JTAG (303A:1001) เลือกพอร์ตนั้น 1 ครั้งต่อกล่อง (Chrome บังคับให้คนเลือกเอง)
  กล่องที่มี firmware อยู่แล้ว (303A:4003) เว็บสั่ง `flash` ให้เข้า download mode แล้วให้กดเพิ่มอีกครั้ง
- Windows 10/11 รู้จักพอร์ตเอง ไม่ต้องลงไดรเวอร์

## รันบน server

```sh
cd flasher/deploy
cp .env.example .env          # LINE_HOST=ชื่อที่ PC ในสายผลิตเปิด เช่น line.factory.local
docker compose up -d --build  # app (8800) + Caddy (https 443)
```

- ข้อมูลทั้งหมดอยู่ใน `deploy/data/` (mount เป็น `/data`): `site.ini`, `mflash.db` (SQLite),
  `library/` (firmware), `debug/` (ไฟล์ debug ต่อ SN), `web.json` (ล็อตและ firmware ที่เลือก)
- `tls internal`: Caddy ออกใบรับรองเอง ต้องให้ PC ในสายเชื่อใบ root ของ Caddy
  (`docker compose exec caddy cat /data/caddy/pki/authorities/local/root.crt`) หรือใช้ใบรับรองของบริษัท
- เดโม: `python -m mflash web --demo` (หรือ `mflash web --demo`) — broker, กล่อง และพอร์ต USB เป็นแบบจำลอง
- ไม่ใช้ Docker: `pip install -r flasher/requirements.txt` แล้ว `cd flasher && python -m mflash.web --host 0.0.0.0`
  หลัง reverse proxy ที่ทำ https (ต้องส่ง WebSocket ต่อด้วย: `/api/devices/*/debug/ws`)
- เปลี่ยน broker / debug broker ในหน้าตั้งค่าแล้ว **รีสตาร์ท app** (ต่อ broker ตอนเริ่ม)

## รวมกับเว็บหลัก

| เรื่อง | ตอนนี้ | ทีม server เปลี่ยนตรงไหน |
|---|---|---|
| Login | **ไม่มี** (ตามที่ตกลง) | ใส่ไว้ที่ reverse proxy หรือ middleware ใน `mflash/web/app.py` `create_app()` · ช่อง "ผู้ทำ" ใช้ชื่อจาก login แทน |
| หน้าเว็บ | `mflash/web/static/` HTML + JS ไม่ต้อง build | ฝังใน layout ของเว็บหลักได้ ทุกอย่างคุยผ่าน `/api/*` |
| API | FastAPI, เอกสารที่ **`/docs`** (OpenAPI) | |
| ฐานข้อมูล | SQLite ผ่าน `mflash/db.py` (คลาส `Db`) | ตาราง: `device` (mac, sn), `run` (ขั้นละแถว: usb, line), `step`, `value` (ค่าต่อเครื่อง), `deleted` |
| ตั้งค่า | `site.ini` ผ่าน `mflash/site.py` | |
| firmware | คลังไฟล์ `library/` + manifest, อนุมัติก่อนใช้ | |
| MQTT | `mflash/mqttlink.py` (paho) login ของเว็บเอง (`[station_mqtt]`) | |

API หลัก (ดูครบที่ `/docs`):

| | |
|---|---|
| `GET /api/config` | ล็อต, เลขเครื่องถัดไป, Wi-Fi ให้เลือก, firmware, ค่าที่หน้าเว็บใช้ flash |
| `POST /api/usb/prepare` | `{mac, lot, unit, wifi[]}` → SN และรายการไฟล์ (URL ใช้ได้ครั้งเดียว, 15 นาที) |
| `GET /api/usb/image/{token}/{name}` | ไฟล์ที่จะเขียน (รวม `nvs.bin`) |
| `POST /api/usb/done` | `{token, ok, log, error}` → บันทึกขั้น `usb` |
| `GET /api/devices` | เครื่องของล็อต + สถานะ + คิวคำถาม |
| `POST /api/line/start` · `/stop/{sn}` · `/answer` · `/auto` | การทดสอบ |
| `POST /api/devices/{sn}/debug` · `WS /api/devices/{sn}/debug/ws` | เปิด/ปิด debug stream และดูสด |
| `GET /api/summary` · `/api/summary.csv` | สรุป SN |
| `POST /api/firmware/import` (zip) · `/approve` · `GET /api/firmware/library` | คลัง firmware |

## ความปลอดภัย (ต้องอ่าน)

- **ไม่มี login**: ใครเข้าถึงเว็บได้ก็ flash และเห็นสถานะได้ ต้องอยู่หลังระบบ login ของเว็บหลัก
- `nvs.bin` ที่เบราว์เซอร์ดาวน์โหลดมี **รหัส Wi-Fi และ login broker** อยู่ข้างใน (กล่องต้องใช้)
  URL ใช้ได้ครั้งเดียว หมดอายุ 15 นาที ส่งผ่าน https เท่านั้น
- MQTT ตอนนี้ 1883 ไม่เข้ารหัส login ร่วมกัน (firmware/PROTOCOL.md §6) ใช้ Wi-Fi โรงงานที่คนนอกเข้าไม่ได้
- รหัสผ่านไม่เคยถูกส่งกลับไปที่หน้าตั้งค่า และถูกปิดใน log/บันทึก

## ขึ้นกับ firmware

การเพิ่มเครื่องใช้ได้กับ firmware ปัจจุบัน (เขียน NVS ตาม key ที่ firmware อ่าน: `mflash/nvsimage.py`)
ขั้นทดสอบใช้คำสั่งผ่าน MQTT (ตกลงกับฝั่ง firmware ใน `notes/flasher.md` และ `notes/firmware.md`):

| ต้องการ | มีแล้ว | ถ้ายังไม่มี |
|---|---|---|
| โหมดโรงงาน `factory/on`, rpc `mcold/v1/<sn>/rpc`, `FACTORY_TEST`, `IDENTIFY`, `GET_DEBUG`, `FACTORY_DONE`, `factory_rev` | **0.7.0-dev.65** | — |
| `DIAG_RUN` (accel, NFC, ชาร์จ, ราง, ไฟไล่สี, เสียง, จอ) | ยัง (ข้อเสนอ) | ใช้ `FACTORY_TEST` + `IDENTIFY` + ให้คนดูหน้าจอปกติ แทน (fallback ใน profile) |
| debug mode: `mcold/v1/<sn>/debug/ctl` (client 1) → stream บน debug broker (client 2) | ยัง (ข้อเสนอ) | ปุ่ม debug ขึ้นว่ากล่องไม่ตอบ |

กล่องที่ firmware ไม่มีคำสั่งที่ต้องใช้จะ**ไม่ผ่าน** ไม่ถูกปล่อยผ่านโดยไม่ได้ทดสอบ
เดโมจำลองทั้งหมดไว้ให้ลองครบวงจร (รวมกล่องที่ยังไม่มี `DIAG_RUN`)

## ยังไม่ได้ลองกับของจริง

- esptool-js 0.5.7 กับบอร์ดจริงผ่าน Web Serial (โค้ดเขียนตาม API ของเวอร์ชันนี้ ทดสอบได้แค่ทางจำลอง)
- NVS ที่สร้าง: ตรวจด้วยตัวอ่านของเราเอง และใช้ตัวสร้างของ Espressif (`esp-idf-nvs-partition-gen`)
  ต้องลองบูตกับบอร์ดว่า `sn`, `wifi`, `mqtt` ขึ้นตามที่เขียน
- Docker image: ไฟล์พร้อม แต่ยัง build ไม่ได้ในเครื่องที่ทำ (ไม่มี Docker daemon) ลองรันจาก layout เดียวกันแล้วใช้ได้

ลองกับบอร์ด: เปิดเว็บ → เพิ่มเครื่อง 1 กล่อง → ที่ console ของกล่อง (`sn`, `wifi`, `mqtt`) ต้องเห็นค่าที่ตั้ง
→ `mosquitto_sub -t 'mcold/<SN>/#'` ต้องเห็น `online` และ `status`
