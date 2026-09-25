# display-mock

ภาพหน้าจอ e-paper ทุก state ที่ขนาดจริง 250×122 · สเปกการออกแบบอยู่ที่
[`../docs/display-design.md`](../docs/display-design.md)

```bash
pip install pillow
python display-mock/render.py      # เขียนทับทุกไฟล์ใน out/
```

`render.py` เป็นต้นฉบับเดียวของ layout — พิกัด, type scale และ bitmap ของไอคอน
อยู่ในไฟล์นี้ทั้งหมด ไฟล์ใน `out/` เป็นผลลัพธ์ commit ไว้เพื่อเปิดดูบน GitHub
ได้โดยไม่ต้องรัน ถ้าแก้ layout ต้องรันใหม่แล้ว commit ภาพไปด้วยกัน

ตัวอักษรเรนเดอร์ผ่าน FreeType แบบ monochrome + hinting (`fontmode = "1"`)
และไอคอนวาดทีละ pixel ในโค้ด ภาพที่ได้จึงมีแค่ 3 สีเท่าที่จอแสดงได้จริง
ไม่มี antialias และไม่มีขอบเทา

## ไฟล์

`<code>.png` = ขนาดจริง 250×122 · `<code>_2x.png` = ขยาย 2 เท่าแบบ nearest neighbour
สำหรับดูบนจอคอม · `contact_sheet.png` = รวมทุกหน้าพร้อมป้ายชื่อ

| Code | State | ไฟล์ |
|---|---|---|
| A1 | IDLE / READY | `A1_idle.png` |
| A2 | TRIP ACTIVE | `A2_trip.png` |
| A3 | OUT OF BAND | `A3_warning.png` |
| A4 | ALARM | `A4_alarm.png` |
| A5 | NO READING | `A5_noreading.png` |
| A6 | CHARGING | `A6_charging.png` |
| B1 | BOOT / SELF-TEST | `B1_boot.png` |
| B2 | USB CONNECTED | `B2_usb.png` |
| B3 | BLE SESSION | `B3_ble.png` |
| B4 | STORAGE FULL | `B4_storage.png` |
| B5 | CRITICAL BATTERY | `B5_battery.png` |
| B6 | FAULT / RECOVERY | `B6_fault.png` |
| C1 | TRIP SUMMARY | `C1_summary.png` |
| C2 | DEVICE DETAIL | `C2_detail.png` |

## ยกไปเป็น firmware

`render.py` แยก layout ออกจาก backend ไว้แล้ว: `Screen` มีแค่ `text` / `box` /
`icon` / `gauge` เมื่อได้ PCB ให้เขียน backend ใหม่ที่วาดลง framebuffer 2 bit/pixel
ของ panel แล้วยกฟังก์ชัน `monitor` / `charge` / `takeover` / `detail`
กับตาราง `ICONS` มาใช้ตรง ๆ พิกัดทุกตัวเป็น baseline ที่ทดสอบแล้วว่าไม่ล้ำเส้นคั่น
