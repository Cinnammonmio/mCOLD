---
name: Quick-Guide
lang: th
version: 0.3
status: draft
date: 2026-10-04
firmware: 0.7.0-dev
compact: yes
---

# mCOLD Foam V.1 — Quick Guide / คู่มือฉบับย่อ

## 1. เปิดเครื่อง · Switch on

เลื่อนสวิตช์เปิดเครื่อง ไฟหน้า 3 ดวงกวาดน้ำเงิน ม่วง ฟ้า แล้วกะพริบฟ้า = พร้อมใช้งาน

Slide the power switch on. The front lights sweep blue, violet, cyan, then blink cyan: ready.

กะพริบเหลือง 3 ครั้งแทน = เครื่องมีปัญหา ดูหัวข้อ 6 · แดงและเหลืองคือการเตือนเสมอ น้ำเงิน ม่วง ฟ้า คือปกติ

Three amber blinks instead: the box has a problem, see 6. Red and amber always warn; blue, violet and cyan are normal.

## 2. ชาร์จ · Charge

เสียบ USB-C · Plug in USB-C

| ไฟข้างเครื่อง · Side light | ความหมาย · Meaning |
|---|---|
| น้ำเงิน หายใจ · breathing blue | กำลังชาร์จ · charging |
| ฟ้า หายใจ · breathing cyan | เกือบเต็ม (≥ 80%) · nearly full |
| ฟ้าค้าง · steady cyan | เต็ม · full |
| ม่วง กะพริบช้า · slow violet blink | มีไฟแต่ไม่ชาร์จ · power in, not charging |
| แดง กะพริบ · red blink | ชาร์จผิดปกติ · charge fault |

## 3. เริ่ม trip · Start a trip

แบตต้องเพียงพอ ถ้าแบตต่ำเครื่องจะไม่ให้เริ่ม trip (ชาร์จก่อน)

The battery must be charged enough; a low battery refuses a new trip.

ไฟกลางกะพริบฟ้า 3 ครั้ง = เริ่ม trip แล้ว

Three cyan blinks on the middle light: the trip has started.

## 4. ระหว่างเดินทาง · During the trip

- เครื่องบันทึกอุณหภูมิทุก 5 นาที · Temperature is recorded every 5 minutes.
- จอแสดงอุณหภูมิล่าสุดและต่ำสุด–สูงสุด · The screen shows the latest temperature with min/max.
- ไฟกลางกะพริบฟ้าสั้นๆ ทุก 15 นาที = ทำงานปกติ · A short cyan blink every 15 min: working normally.

## 5. Alarm อุณหภูมิ · Temperature alarm

ไฟซ้ายกะพริบแดง 2 ครั้ง + เสียงบี๊บ 3 ครั้ง และกรอบแดงบนจอ = อุณหภูมิออกนอกช่วงที่ตั้งไว้

Two red blinks on the left light, three beeps and a red frame on the screen: the temperature left its range.

## 6. ไฟเหลือง · Amber light

ไฟขวากะพริบเหลือง 3 ครั้ง = ปัญหาของเครื่อง ไม่ใช่สินค้า (เช่น probe หลุด, แบตต่ำ)

Three amber blinks on the right light: a problem with the box, not the goods (probe, battery).

## 7. แบตหมด · Battery empty

จอขึ้น **BATTERY EMPTY** และเครื่องดับเอง ข้อมูลไม่หาย เสียบ USB-C แล้วเครื่องจะกลับมาทำงานต่อ

The screen shows **BATTERY EMPTY** and the box switches itself off. No data is lost; plug in USB-C and it carries on.

---

### ยังไม่รวม · Not yet included

- ขั้นตอนเริ่ม/หยุด trip และ ack alarm ในแอป (แอปยังไม่เสร็จ)
- ระยะเวลาใช้งานต่อการชาร์จ, เวลาชาร์จเต็ม (รอผลวัด)
- ภาพเครื่องและตำแหน่งปุ่ม/พอร์ต/จุดแตะ NFC (รอภาพเครื่องจริง)
