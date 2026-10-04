# display-mock

ภาพหน้าจอ e-paper ทุก state ที่ขนาดจริง 250×122 · สเปกการออกแบบอยู่ที่
[`../docs/display-design.md`](../docs/display-design.md)

```bash
pip install pillow
python display-mock/render.py      # เขียนทับทุกไฟล์ใน out/
```

`render.py` เป็นต้นฉบับของ layout — พิกัด, type scale และทุก template อยู่ที่นี่
ส่วน `icons.py` เป็นต้นฉบับของไอคอน footer โดยมี `ICON_H` เป็นปุ่มเดียวที่คุมขนาด
ทุกตัว ไฟล์ใน `out/` เป็นผลลัพธ์ commit ไว้เพื่อเปิดดูบน GitHub ได้โดยไม่ต้องรัน
ถ้าแก้ layout ต้องรันใหม่แล้ว commit ภาพไปด้วยกัน

```bash
python display-mock/icons.py       # แถบพรีวิวไอคอน + นิยาม C สำหรับ firmware
```

ไอคอนถูก generate จากเรขาคณิตแล้ว rasterise ผ่าน mask ที่ supersample ไว้ ไม่ได้
วาดทีละ pixel ด้วยมือ เพราะที่ขนาดนี้รูปโค้งที่พิมพ์เอาเองออกมาเบี้ยวโดยมองไม่ออก
ว่าเบี้ยวตรงไหน · `icons.py` เป็นที่เดียวที่ผลิตบิตแมปเหล่านี้ ทั้งภาพ mockup และ
นิยาม C ที่ firmware คอมไพล์ — การมีสองไฟล์ที่ผลิตบิตแมปเดียวกันเคยทำให้ header
ของไอคอนไม่ตรงกับจำนวนแถวจริง ซึ่งบนบอร์ดแปลว่าอ่านทะลุปลาย array

ตัวอักษรเรนเดอร์ผ่าน FreeType แบบ monochrome + hinting (`fontmode = "1"`)
และไอคอนวาดทีละ pixel ในโค้ด ภาพที่ได้จึงมีแค่ 3 สีเท่าที่จอแสดงได้จริง
ไม่มี antialias และไม่มีขอบเทา

## ไฟล์

`<code>.png` = ขนาดจริง 250×122 · `<code>_2x.png` = ขยาย 2 เท่าแบบ nearest neighbour
สำหรับดูบนจอคอม · `contact_sheet.png` = รวมทุกหน้าพร้อมป้ายชื่อ ·
`footer_icons.png` = ไอคอน footer ทุกตัวเรียงกันที่ความสูงเดียวกัน

| Code | State | ไฟล์ |
|---|---|---|
| A1 | READY — ไม่มี trip: "SCAN TO START TRIP" | `A1_idle.png` |
| A1b | READY แบตต่ำ (เริ่ม trip ไม่ได้) | `A1b_idle_low.png` |
| A1c | READY ระหว่างชาร์จ | `A1c_idle_charging.png` |
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
