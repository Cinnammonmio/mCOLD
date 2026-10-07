# เอกสารสำหรับทีมแอป

กล่องคุยกับแอปผ่าน **NFC (แตะเพื่อยืนยันตัว) และ BLE** ไฟล์ทั้งหมดมี 2 รูปแบบ: `.md` (ต้นฉบับ แก้ผ่าน git) และ `.pdf` (ในโฟลเดอร์ [`pdf/`](pdf/) สำหรับอ่านและแจก)

| เอกสาร | เนื้อหา | PDF |
|---|---|---|
| [`ble-protocol.md`](ble-protocol.md) | หากล่องด้วย NFC, GATT, AUTH, รูปแบบข้อความ, คำสั่งทั้งหมด, `trip_id`, Wi-Fi 5 ช่อง, วิธีที่แอปเห็นค่าที่เปลี่ยน, ส่ง trip เป็น CSV | [`pdf/`](pdf/) |
| [`device-settings.md`](device-settings.md) | ค่าตั้งของกล่อง ใครตั้งได้ ช่วงค่า อะไรล็อกระหว่าง trip | [`pdf/`](pdf/) |

เกี่ยวกับ server: [`../server/`](../server/README.md) (โดยเฉพาะ [`trip-data-flow.md`](../server/trip-data-flow.md) ว่าข้อมูล trip ไปทางไหน)

## สำหรับคนดูแลเอกสาร

- แก้ที่ `.md` แล้วสร้าง PDF ใหม่ด้วย `python docs/manuals/build.py docs/app/ble-protocol.md` (ต้องมี Python 3 และ Microsoft Edge) ไฟล์ PDF ใหม่อยู่ใน `docs/app/pdf/` และชื่อไฟล์มีเลขเวอร์ชันจากหัวไฟล์ (`version:`) เพิ่มเลขเมื่อเนื้อหาเปลี่ยน
- เนื้อหาตรงกับ firmware เวอร์ชันที่ระบุในหัวไฟล์ (`firmware:`)
