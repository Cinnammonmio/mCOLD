---
name: MQTT-Topics
lang: th
version: 1.0
date: 2026-10-05
firmware: 0.7.0-dev.4
---

# mCOLD — MQTT topics

สรุปทุก topic ที่กล่อง mCOLD ใช้ ว่าใคร publish ใคร subscribe และข้อมูลหน้าตาเป็นอย่างไร
รายละเอียดสำหรับทีม server อยู่ที่ [`briefs/server-mqtt-brief.md`](briefs/server-mqtt-brief.md)
และสเปกเต็มภาษาอังกฤษอยู่ที่ [`../firmware/PROTOCOL.md`](../firmware/PROTOCOL.md) ข้อ 6

## หลักการตั้งชื่อ (แบบเดียวกับ eTEMP)

| | รูปแบบ | ตัวอย่าง |
|---|---|---|
| กล่อง **publish** | `mcold/<sn>/...` (ไม่มี v1) | `mcold/mCDV1-L0169-1069-001/rec` |
| กล่อง **subscribe** (server publish) | `mcold/v1/<sn>/...` | `mcold/v1/mCDV1-L0169-1069-001/ack` |

`v1` คือเวอร์ชันของคำสั่งที่กล่องเข้าใจ ถ้าวันหน้าคำสั่งเปลี่ยนแบบที่กล่องเก่าไม่เข้าใจ
ให้ server ส่งที่ `mcold/v2/<sn>/...` กล่องสองรุ่นรันบน broker เดียวกันได้ระหว่างทยอยอัปเดต

## การเชื่อมต่อ

- broker, พอร์ต และ login ตั้งในเครื่อง (NVS) ด้วยคำสั่ง `mqtt set` ไม่ฝังใน firmware
- client id = SN ของกล่อง
- last will: `mcold/<sn>/online` = `"0"` (retain) ถ้ากล่องหลุดโดยไม่บอก
- ตอนเสียบไฟ: ต่อค้างตลอด
- ตอนใช้แบต: เปิดเป็นรอบ ๆ (session) ตาม `upload_period_s` (ค่าเริ่มต้น 5 นาที) เมื่อมีข้อมูลรอส่ง
  และเช็คอินทุก 6 ชั่วโมงแม้ไม่มีข้อมูล ในแต่ละ session กล่องรอ ACK ได้ไม่เกิน 10 วินาที

## Topics

| Topic | กล่อง | Server | QoS | Retain | เนื้อหา |
|---|---|---|---|---|---|
| `mcold/<sn>/rec` | **pub** | **sub** | 1 | ไม่ | ชุดแถว log ไม่เกิน 16 แถว |
| `mcold/<sn>/status` | **pub** | **sub** | 0 | ใช่ | สถานะล่าสุดของกล่อง |
| `mcold/<sn>/online` | **pub** + last will | **sub** | 1 | ใช่ | `"1"` ต่ออยู่ / `"0"` จบ session หรือหลุด |
| `mcold/<sn>/ota/state` | **pub** | **sub** | 1 | ใช่ | ผลการอัปเดต firmware |
| `mcold/v1/<sn>/ack` | **sub** | **pub** | 1 | ไม่ | ยืนยันว่าบันทึกแถวแล้ว |
| `mcold/v1/<sn>/firmware` | **sub** | **pub** | 1 | **ใช่** | ชื่อไฟล์ firmware ที่จะให้อัปเดต |

**Server subscribe:** `mcold/+/rec`, `mcold/+/status`, `mcold/+/online`, `mcold/+/ota/state`
(หรือ `mcold/#` แล้วกรองเอง)

**Server publish:** `mcold/v1/<sn>/ack` และ `mcold/v1/<sn>/firmware`

## ข้อมูลในแต่ละ topic

### `mcold/<sn>/rec` — แถว log

```json
{"sn":"mCDV1-L0169-1069-001","trip":11,"schema":3,"from":0,"to":15,
 "rows":[
  {"trip":11,"seq":1,"sn":"mCDV1-L0169-1069-001","timestamp":"02:47:27 05/10/2026",
   "utc":1791143247,"event":"SAMPLE","temp":4.25,"tempmin":2,"tempmax":8,"alarm":"",
   "timeok":true,"gnssstate":"last","latitude":13.7563,"longitude":100.5018,
   "motion":0,"battery":97,"internet":"online","detail":""}, ...]}
```

ทุกแถวมี column ชุดเดียวกัน ไม่ว่าจะเป็น sample หรือ event · **key คือ (sn, trip, seq)**
ความหมายของแต่ละ column และรายการ event อยู่ใน brief ข้อ 3

### `mcold/v1/<sn>/ack` — server ตอบ

```json
{"trip":11,"upto":15}
```

แปลว่าแถว `seq` 0–15 ของ trip 11 บันทึกลงฐานข้อมูลแล้วทั้งหมด (ต้องต่อเนื่อง ไม่มีช่องว่าง)
ส่งหลังบันทึกเสร็จเท่านั้น และควรส่งภายใน 1–2 วินาที กล่องจะไม่ลบแถวที่ยังไม่ได้ ACK

### `mcold/<sn>/status` — สถานะล่าสุด

```json
{"time":{"utc":1791143247,"quality":"ntp","boot":42},
 "temp":{"ok":true,"c":4.25},
 "trip":{"active":true,"id":11,"samples":12,"min":3.5,"max":5.25,"alarms":[],"acked":false},
 "power":{"soc":78,"mv":3987,"ma":-12,"charge":"none","external":false},
 "gnss":{"fix":false,"lat":null,"lon":null,"age_s":null},
 "storage":{"used_pct":1},
 "fw":{"ver":"0.7.0-dev.4","slot":"ota_0"},
 "net":{"connected":true,"ssid":"Office","rssi":-59,"ip":"192.168.1.147","mac":"28:84:85:27:9A:74"}}
```

ส่งทุกครั้งที่ต่อได้ (ตอนใช้แบตคือทุก session) ใช้ดูว่ากล่องติดต่อมาล่าสุดเมื่อไร
ดีกว่าดู `online` เพราะกล่องที่ใช้แบตจะเป็น `"0"` เกือบตลอดซึ่งเป็นเรื่องปกติ

### `mcold/v1/<sn>/firmware` — สั่งอัปเดต

payload คือชื่อไฟล์อย่างเดียว เช่น `mCOLD_0.7.1.bin` (publish แบบ **retain**)
กล่องดาวน์โหลดจาก `https://drive.siamatic.co.th/media/firmwares/<ชื่อไฟล์>` แล้วตรวจลายเซ็นเอง
ได้ `ok` แล้วให้ลบ retain (publish ค่าว่างแบบ retain)

### `mcold/<sn>/ota/state` — ผลการอัปเดต

```json
{"ver":"0.7.1","state":"downloading","pct":40}
{"ver":"0.7.1","state":"rebooting","from":"0.7.0"}
{"ver":"0.7.1","state":"ok","from":"0.7.0"}
{"file":"mCOLD_0.7.1.bin","state":"failed","reason":"..."}
{"file":"mCOLD_0.7.1.bin","state":"deferred","reason":"trip"}
{"ver":"0.7.1","state":"rolled_back","running":"0.7.0"}
```

### `mcold/<sn>/online`

`"1"` ตอนต่อได้, `"0"` ตอนจบ session หรือ broker ส่งแทนเมื่อกล่องหลุด (last will)

## สิทธิ์ (ACL) ที่ควรตั้งก่อนใช้งานจริง

- กล่องแต่ละตัว publish ได้เฉพาะ `mcold/<sn>/#` ของตัวเอง
- กล่องแต่ละตัว subscribe ได้เฉพาะ `mcold/v1/<sn>/#` ของตัวเอง
- `mcold/v1/#` ให้ publish ได้เฉพาะ server: ACK คือคำสั่งให้กล่องลบข้อมูล และ firmware
  คือคำสั่งอัปเดต ถ้าใครปลอมได้ ข้อมูลที่ยังไม่ถึง server จะหาย
