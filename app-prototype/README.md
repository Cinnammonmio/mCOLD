# ต้นแบบแอป Cold Chain Hub (ส่วน mCOLD)

เว็บแอปต้นแบบที่กดใช้งานได้ จำลองแอปมือถือที่รวมหลายผลิตภัณฑ์ (eTEMP, mCOLD, SMT) โดยออกแบบละเอียดเฉพาะ mCOLD
ใช้นำเสนอทีมพัฒนาแอปให้เห็นหน้าจอ ลำดับการใช้งาน และพฤติกรรมของระบบ

- **ข้อมูลจำลองทั้งหมด** ไม่มีการเรียก Web Bluetooth, Web NFC, API หรือ Server จริง ไม่ต้องใช้ API key
- สถานะเก็บใน `localStorage` ของเบราว์เซอร์ รีเซ็ตได้จากแผง **ควบคุมเดโม**
- **คู่มือสำหรับทีมพัฒนา** อยู่ในต้นแบบ (ปุ่มในแผงควบคุมเดโม หรือเปิด `index.html#guide`):
  แผนผังหน้าจอ, Flow, ตารางหน้าจอ/การกระทำ/ผลลัพธ์/กรณีผิดพลาด, mapping คำสั่ง BLE,
  สิ่งที่อ้างอิงเอกสาร / ข้อเสนอ UX / ประเด็นที่ต้องตกลง และข้อขัดแย้งที่พบในเอกสาร
- ชื่อ “Cold Chain Hub” เป็นชื่อชั่วคราว

## เปิดดู

ไฟล์ที่ build แล้วอยู่ที่ [`dist/index.html`](dist/index.html) เป็นไฟล์เดียว เปิดในเบราว์เซอร์ได้เลย
(ฟอนต์ IBM Plex Sans Thai โหลดจาก Google Fonts ถ้าออฟไลน์จะใช้ฟอนต์ไทยของระบบแทน)

บน Desktop แสดงเป็นกรอบมือถือพร้อมแผงควบคุมเดโมด้านขวา บนมือถือแสดงเต็มจอ และแผงเดโมอยู่ที่ปุ่ม “เดโม”

## พัฒนาต่อ

```bash
cd app-prototype
npm install
npm run dev      # เปิด http://localhost:5173
npm run build    # สร้าง dist/index.html ไฟล์เดียว
```

ต้องมี Node.js 18 ขึ้นไป · React 18 + Vite 5

## โครงสร้าง

```
src/
  store.js          สถานะทั้งหมด, ข้อมูลตั้งต้น 4 เครื่อง, เก็บ/โหลด localStorage, navigation, toast
  sim.js            กล่องและ server จำลอง: โมเดลอุณหภูมิ, คำสั่งแบบ async, ขั้นตอนส่งข้อมูล 5 ขั้น, ปุ่มเดโม
  format.js         รูปแบบตัวเลข วันที่ ระยะเวลา (ค่าที่ไม่รู้แสดง "—" ไม่ใช่ 0)
  ui.jsx            ปุ่ม ชิป แบนเนอร์ sheet ขั้นตอน ฯลฯ
  icons.js          path ของ Tabler Icons (MIT) ที่ใช้
  ConnectSheet.jsx  แตะ NFC → พบอุปกรณ์ → BLE → ยืนยันสิทธิ์ → พร้อมสั่งงาน
  DemoPanel.jsx     แผงควบคุมเดโม (แยกจากหน้าผู้ใช้)
  Guide.jsx         คู่มือสำหรับทีมพัฒนา
  screens/
    Products.jsx    หน้าเลือกผลิตภัณฑ์ + หน้าสำรอง eTEMP/SMT
    Mcold.jsx       เปลือก mCOLD, แท็บอุปกรณ์, ตัวเลือกเครื่อง
    Trip.jsx        แท็บทริป: ไม่มีทริป / ระหว่างทริป, กราฟ
    TripFlows.jsx   เริ่มทริป 3 ขั้น, จบทริป
    Settings.jsx    ตั้งค่าอุปกรณ์, Wi-Fi 5 ช่อง
    Send.jsx        เลือกและส่งข้อมูล, หน้าความคืบหน้า
```

## เอกสารอ้างอิง

[`docs/app/ble-protocol.md`](../docs/app/ble-protocol.md) ·
[`docs/app/device-settings.md`](../docs/app/device-settings.md) ·
[`docs/server/trip-data-flow.md`](../docs/server/trip-data-flow.md) ·
[`docs/mCOLD_Firmware_Requirements_2026-09-19.md`](../docs/mCOLD_Firmware_Requirements_2026-09-19.md)

เอกสาร BLE ยังเป็น Draft พฤติกรรมที่ต้นแบบเสนอเอง (เช่น คิวในแอป, รับข้อมูลต่อจากจุดที่หลุด, เลือกส่งหลายทริป)
ระบุไว้ในคู่มือว่าเป็น “ข้อเสนอ UX” ไม่ได้อ้างว่า firmware รองรับแล้ว
