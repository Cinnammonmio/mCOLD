# เอกสารสำหรับทีม server

กล่องคุยกับ server ผ่าน **MQTT** ไฟล์ทั้งหมดมี 2 รูปแบบ: `.md` (ต้นฉบับ แก้ผ่าน git) และ `.pdf` (ในโฟลเดอร์ [`pdf/`](pdf/) สำหรับอ่านและแจก)

| เอกสาร | เนื้อหา | PDF |
|---|---|---|
| [`server-mqtt-brief.md`](server-mqtt-brief.md) | สิ่งที่ server ต้องทำ เรียงตามความสำคัญ: ACK, รูปแบบแถว, OTA, การตั้งค่า, ความปลอดภัย, คำถามที่ต้องตอบ | [`pdf/`](pdf/) |
| [`mqtt-topics.md`](mqtt-topics.md) | ทุก topic ใครส่งใครรับ ตัวอย่างข้อความ | [`pdf/`](pdf/) |
| [`trip-data-flow.md`](trip-data-flow.md) | ข้อมูล trip ไปไหนบ้าง: ระหว่าง trip, จบ trip, ส่งทาง Wi-Fi, ส่งทางแอป | [`pdf/`](pdf/) |

เกี่ยวกับแอป: [`../app/`](../app/README.md) ค่าตั้งของกล่องที่ server ส่งได้ (เอกสาร `config`) อยู่ที่ [`../app/device-settings.md`](../app/device-settings.md)

## สำหรับคนดูแลเอกสาร

- แก้ที่ `.md` แล้วสร้าง PDF ใหม่ด้วย `python docs/manuals/build.py docs/server/server-mqtt-brief.md` (ต้องมี Python 3 และ Microsoft Edge) ไฟล์ PDF อยู่ใน `docs/server/pdf/` ชื่อไฟล์มีเลขเวอร์ชันจากหัวไฟล์ (`version:`)
- เนื้อหาตรงกับ firmware เวอร์ชันที่ระบุในหัวไฟล์ (`firmware:`)
