---
name: MQTT-Topics
lang: th
version: 1.1
date: 2026-10-05
firmware: 0.7.0-dev.35
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
| `mcold/<sn>/rec` | **pub** | **sub** | 1 | ไม่ | แถว log ของ trip **ที่จบแล้ว** ก้อนละ 20 แถว |
| `mcold/<sn>/status` | **pub** | **sub** | 0 | ใช่ | สถานะล่าสุดของกล่อง |
| `mcold/<sn>/online` | **pub** + last will | **sub** | 1 | ใช่ | `"1"` ต่ออยู่ / `"0"` จบ session หรือหลุด |
| `mcold/<sn>/ota/state` | **pub** | **sub** | 1 | ใช่ | ผลการอัปเดต firmware |
| `mcold/v1/<sn>/ack` | **sub** | **pub** | 1 | ไม่ | ยืนยันว่าบันทึกแถวแล้ว |
| `mcold/v1/<sn>/firmware` | **sub** | **pub** | 1 | **ใช่** | ชื่อไฟล์ firmware ที่จะให้อัปเดต |
| `mcold/v1/<sn>/config` | **sub** | **pub** | 1 | **ใช่** | เอกสารตั้งค่า (Wi-Fi + DHCP/IP คงที่, รอบส่ง, ไฟ, temp adjust …) |
| `mcold/<sn>/config/state` | **pub** | **sub** | 1 | ใช่ | ผลการตั้งค่า |

**Server subscribe:** `mcold/+/rec`, `mcold/+/status`, `mcold/+/online`, `mcold/+/ota/state`,
`mcold/+/config/state`
(หรือ `mcold/#` แล้วกรองเอง)

**Server publish:** `mcold/v1/<sn>/ack`, `mcold/v1/<sn>/firmware` และ `mcold/v1/<sn>/config`

## ข้อมูลในแต่ละ topic

### `mcold/<sn>/rec` — แถว log

```json
{"sn":"mCDV1-L0169-1069-002","trip_id":"95518e06-1b3b-494f-b4c5-284ae94f5424","trip_date":"20261007","trip_number":2,"schema":5,"part":1,"parts":1,
 "from":0,"to":6,"last":true,
 "rows":[
  {"trip_id":"95518e06-1b3b-494f-b4c5-284ae94f5424","trip_date":"20261007","trip_number":2,"seq":1,"timestamp":1791143247,"event":"SAMPLE","temp":4.25,"tempmin":2,"tempmax":8,"alarm":"",
   "timeok":true,"gnssstate":"last","latitude":13.7563,"longitude":100.5018,
   "motion":0,"battery":97,"internet":"online"}, ...]}
```

ส่งเฉพาะ trip ที่จบแล้ว ก้อนละ 20 แถว (ก้อนที่ k = `seq` 20(k−1)…20k−1) · `last: true` คือก้อนสุดท้าย
ระหว่าง trip ส่งแค่ `status` — ดู [`trip-data-flow.md`](trip-data-flow.md)
ทุกแถวมี column ชุดเดียวกัน ไม่ว่าจะเป็น sample หรือ event · **key คือ (trip_id, seq)** (`trip_id` เป็น UUID ที่กล่องสร้าง, `sn` อยู่ที่หัวชุดและใน topic) · แถวใน MQTT ไม่มี `sn`, `utc`, `detail` และ `timestamp` เป็นวินาที Unix
ความหมายของแต่ละ column และรายการ event อยู่ใน brief ข้อ 3

### `mcold/v1/<sn>/ack` — server ตอบ

```json
{"trip_id":"95518e06-1b3b-494f-b4c5-284ae94f5424","upto":15}
```

แปลว่าแถว `seq` 0–15 ของ trip 11 บันทึกลงฐานข้อมูลแล้วทั้งหมด (ต้องต่อเนื่อง ไม่มีช่องว่าง)
ส่งหลังบันทึกเสร็จเท่านั้น และควรส่งภายใน 1–2 วินาที กล่องจะไม่ลบแถวที่ยังไม่ได้ ACK

### `mcold/<sn>/status` — สถานะล่าสุด

```json
{"time":{"utc":1791143247,"quality":"ntp","boot":42},
 "temp":{"ok":true,"c":4.25},
 "trip":{"active":true,"trip_id":"95518e06-1b3b-494f-b4c5-284ae94f5424","trip_date":"20261007","trip_number":2,"samples":12,"min":3.5,"max":5.25,"alarms":[],"acked":false,
         "alarms_raised":1,"last_alarm":{"type":"HIGH","utc":1791146150}},
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

### `mcold/v1/<sn>/config` — ตั้งค่า (retain)

```json
{"rev":3,"config":{"upload_period_s":600,"temp_adj_c100":-50},
 "wifi":{"add":[{"ssid":"Warehouse-2","pass":"...","dhcp":false,"ip":"192.168.1.50",
                 "gateway":"192.168.1.1","subnet":"255.255.255.0","dns":"8.8.8.8"}]}}
```

`rev` เพิ่มขึ้นทุกครั้ง กล่องใช้แต่ละ rev ครั้งเดียว · MQTT broker ตั้งทางนี้ไม่ได้ (console เท่านั้น)
· รายการค่าทั้งหมดและกติกาอยู่ที่
[`device-settings.md`](device-settings.md) · ผลส่งกลับที่ `mcold/<sn>/config/state`

```json
{"rev":3,"applied":["upload_period_s","wifi add Warehouse-2"],"errors":{},"state":"ok"}
```

### `mcold/<sn>/online`

`"1"` ตอนต่อได้, `"0"` ตอนจบ session หรือ broker ส่งแทนเมื่อกล่องหลุด (last will)

## สิทธิ์ (ACL) ที่ควรตั้งก่อนใช้งานจริง

- กล่องแต่ละตัว publish ได้เฉพาะ `mcold/<sn>/#` ของตัวเอง
- กล่องแต่ละตัว subscribe ได้เฉพาะ `mcold/v1/<sn>/#` ของตัวเอง
- `mcold/v1/#` ให้ publish ได้เฉพาะ server: ACK คือคำสั่งให้กล่องลบข้อมูล และ firmware
  คือคำสั่งอัปเดต ถ้าใครปลอมได้ ข้อมูลที่ยังไม่ถึง server จะหาย
