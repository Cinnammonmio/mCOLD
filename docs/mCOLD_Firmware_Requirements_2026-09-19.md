# FOAM: Hardware, Functions and Claude Firmware Prompt

Revision: 2026-09-19 (Flash primary storage + Wi-Fi synchronization + USB MSC/CSV)
Language: Thai requirements; English code identifiers
Target: Foam Device foamV1.0.1 + Foam Dock P.3 (6 slots)
Display confirmed by owner: Waveshare 2.13inch e-Paper (G), raw display, 250×122, Red/Yellow/Black/White

เอกสารนี้เป็นพรอมป์ต์ฉบับเต็มสำหรับส่งให้ Claude พัฒนา Firmware
ให้แนบ PDF วงจรทั้งสองฉบับพร้อมเอกสารนี้ เพื่อให้ตรวจสายและสัญลักษณ์ประกอบได้
ยังไม่มีการทดลองต้นแบบหรือยืนยัน runtime/ความแม่นยำจากการวัดจริง

## 0. บทบาทและวิธีใช้ข้อมูล

คุณคือ Senior Embedded Firmware Engineer ที่เชี่ยวชาญ ESP32-S3, ESP-IDF, low-power data logger, USB, BLE, GNSS และ battery power management

ช่วยพัฒนา Firmware สำหรับโปรเจกต์ Foam ตามรายละเอียดด้านล่าง โดยเริ่มจากตรวจ requirement และ pin map แล้วทำโครงสร้างโปรเจกต์และส่วนที่พัฒนาได้จริงต่อทันที ไม่ต้องรอข้อมูลทุกอย่างจนครบจึงเริ่มงาน

ใช้สถานะข้อมูลดังนี้:

- [HW] พบใน schematic ที่แนบ เป็นข้อเท็จจริงของวงจรในเอกสาร ไม่ใช่หลักฐานว่าบอร์ดจริงทำงานผ่านแล้ว
- [REQ] ความต้องการของเจ้าของโปรเจกต์ที่เคยระบุ
- [PROPOSED] ข้อเสนอเพื่อทำให้ Firmware เริ่มพัฒนาได้ ยังไม่ใช่การตัดสินใจเดิมของเจ้าของ
- [VERIFY] ข้อมูลไม่ครบ ความขัดแย้ง หรือข้อจำกัดที่ต้องตรวจเพิ่มเติม

ข้อกำหนดที่เจ้าของยืนยันเพิ่มเติม 2026-09-19:

- Flash ของ ESP32 เป็นพื้นที่บันทึกหลักและต้องออกแบบ partition ให้รองรับข้อมูลออฟไลน์อย่างน้อย 1 เดือน
- SD card เป็นอุปกรณ์เสริม มีหรือไม่มีก็เริ่ม Trip และใช้งานได้; ใช้เป็นพื้นที่สำรอง/รับข้อมูลเมื่อ Flash ต้องคืนพื้นที่
- ระหว่าง Trip ที่เชื่อม Wi-Fi และเข้าถึง server ได้ ให้ส่งข้อมูลใหม่ต่อเนื่องตามข้อมูลที่เกิดขึ้น; เมื่อ offline ให้เก็บใน Flash แล้วทยอยส่งย้อนหลังเมื่อ online
- คืนพื้นที่หลัง server ยืนยันรับข้อมูล; หากพื้นที่ไม่พอ ให้จัดการทริปเก่าก่อนและทิ้งทีละทริป โดยย้ายไป SD ก่อนเมื่อมี SD ที่เขียนได้
- เสียบ Notebook/PC แล้วต้องเห็นเป็น USB Flash Drive และคัดลอกไฟล์ CSV จากข้อมูลที่ยังเก็บอยู่ในตัวเครื่องได้ โดยไม่ต้องมี SD

ข้อกำหนดชุดนี้แทนข้อความรุ่นก่อนที่ให้ SD เป็น storage หลัก หรือให้ Wi-Fi/USB MSC เป็นเพียง optional

ลำดับความน่าเชื่อถือ:

0. ข้อมูลล่าสุดจากเจ้าของข้างต้นมีผลกับ storage/network/USB; ส่วนจอใช้ Waveshare 2.13inch e-Paper (G) แบบ raw display 4 สี 250×122 แทนข้อกำหนดจอรุ่นก่อนทั้งหมด; revision ของแผงจริงยังต้องตรวจ
1. PDF สองฉบับด้านล่างสำหรับ part/net/GPIO จริง
2. ความต้องการ [REQ] สำหรับพฤติกรรมของผลิตภัณฑ์
3. ข้อเสนอ [PROPOSED] ที่ปรับได้ผ่าน configuration
4. หาก hardware ทำ requirement ไม่ได้ ให้รายงานข้อจำกัด อย่าแต่งวงจรหรือ GPIO ขึ้นมา

เอกสารอ้างอิงหลัก:

- Device: SCH_Schematic_foamV1.0.1_P.1_2026-09-18.pdf, 1 หน้า, title Schematic_foamV1.0.1_P.1, sheet P1_M80, Update Date 2026-09-18
- Dock: SCH_Schematic_foam_dock_P.3_2026-09-18.pdf, 1 หน้า, title Schematic_foam_dock_P.3, sheet P1
- หมายเหตุ: ชื่อไฟล์ Dock ลงวันที่ 2026-09-18 แต่ title block ภายในยังระบุ Update Date 2026-09-13 ให้ใช้วงจรภายในไฟล์แนบนี้เป็นหลัก

อย่าเอาวงจรรุ่นก่อน เช่น TP5000, LiFePO4, Dock ESP32-C3, UART บน D+/D-, FUSB302, buck TPS54302 แยก 6 ตัว หรือ Dock LED กลับมาปนกับรุ่นนี้

## 1. ภาพรวมผลิตภัณฑ์

[REQ] Foam เป็นอุปกรณ์ติดตามกล่องโฟมขนส่งยา เน้นใช้แบตเตอรี่และประหยัดพลังงาน มี Device ติดกล่องและ Dock สำหรับชาร์จ/ดึงข้อมูลหลายเครื่อง

เป้าหมายหลัก:

- แบตเตอรี่ Li-Po 1 เซลล์ ความจุเป้าหมาย 1500 mAh
- ใช้งานเป้าหมาย 7 วัน
- โหมด offline ประหยัดไฟใช้รอบเดิม 5 นาที; เมื่อ Trip online ให้ส่ง sample/event ใหม่ทันทีตามหัวข้อ 9 และ 10
- ใช้ NFC ร่วมกับ BLE เพื่อระบุตัวเครื่อง เชื่อมแอป และเริ่ม Trip
- ใช้กับแอปส่วนตัวบน iOS ที่รองรับ NFC + BLE
- Dock รุ่นปัจจุบันรองรับ 6 เครื่อง ไม่มี MCU และไม่มี LED บน Dock
- นโยบายชาร์จล่าสุด: Dock เป้าหมาย 1.2 A; แหล่งอื่นเป้าหมาย 0.5 A
- ต้องเสียบ USB กับคอมพิวเตอร์/Notebook โดยตรงแล้วเห็นไดรฟ์ที่มีไฟล์ CSV ได้ แม้ไม่มี SD card
- ใช้ Flash ภายในเป็น persistent log หลัก รองรับ offline backlog 31 วันตาม capacity profile ที่ประกาศ; SD เป็น optional overflow/backup
- เมื่อ server ยืนยันรับข้อมูลแล้วให้ทยอยคืนพื้นที่; เมื่อ Flash เต็มให้ย้ายทริปเก่าไป SD หรือทิ้งทริปเก่าตามนโยบาย
- ต้องการปิดสวิตช์เครื่องแล้วชาร์จ 5 V ต่อได้; เมื่อเปิดเครื่องจึงให้ Firmware จัดการ PD 9 V ตามเงื่อนไขที่ยืนยันแล้ว

[PROPOSED] ขอบเขตฟังก์ชันใช้งานที่ควรทำให้ครบตามอุปกรณ์ที่มี:

- บันทึกอุณหภูมิ ประตูเปิด/ปิด การเคลื่อนไหว/แรงกระแทก พิกัด เวลา แบตเตอรี่ และเหตุการณ์
- ทำงานและบันทึกต่อได้เมื่อไม่มีมือถือ/เครือข่าย
- แสดงสถานะบน e-paper, LED และ buzzer
- ตั้งค่า/เริ่มหยุด Trip/ดูสถานะ/ดาวน์โหลดข้อมูลผ่าน BLE และ USB
- Wi-Fi เป็นช่องทางหลักที่ยืนยันแล้วสำหรับ server synchronization; endpoint และ application protocol ยังต้องระบุ
- จัดการการชาร์จและ low-power state อย่างชัดเจน
- รองรับการวินิจฉัยอุปกรณ์และอัปเดต Firmware ตามหัวข้อด้านล่าง

[VERIFY] ยังต้องยืนยัน sample rate ของแต่ละ sensor, event budget, เกณฑ์ alarm, หน้าจอ, BLE UUID, server endpoint/API/ACK และวิธีระบุ Dock โดยไม่มี PC; storage หลัก, Wi-Fi synchronization และ USB MSC/CSV ได้รับการยืนยันแล้ว

## 2. Hardware ฝั่ง Device

### 2.1 ชิ้นส่วนหลัก

| ส่วน | Part / รายละเอียด [HW] | งาน Firmware |
|---|---|---|
| MCU | ESP32-S3-WROOM-1U-N16R8 | Firmware หลัก, USB, BLE, Wi-Fi |
| Memory ของ module | Flash 16 MB, PSRAM 8 MB แบบ Octal | ตั้ง build configuration ให้ตรง N16R8 |
| Temperature | U14 MAX6675ISA+T + thermocouple K | อ่านอุณหภูมิและ probe-open fault |
| Accelerometer | U11 LIS2DW12TR | 3-axis acceleration, motion/wake/shock event |
| GNSS | CN1 ATGM336H-6N-74 | UART, position/time/fix status |
| GNSS antenna | GPS1003 และทางเลือก RF connector ผ่าน matching/selection parts | ไม่มี GPIO สำหรับสลับ antenna |
| Primary storage [REQ] | Flash 16 MB ของ ESP32 module | custom partitions และ persistent Trip log หลัก |
| Optional storage [REQ] | CARD2 microSD/TF แบบ SPI | รับทริปจาก Flash เพื่อสำรอง/ขยายพื้นที่; ไม่บังคับติดตั้ง |
| NFC | U21 ST25DV04KC-IE6S3 | Dynamic NFC tag, NDEF, GPO wake/event |
| RTC | U16 PCF8523T/1,118 + crystal 32.768 kHz | เวลาและปฏิทิน |
| Battery gauge | U6 MAX17048G+T10 | SOC และ cell voltage |
| Current monitor | U13 INA226AIDGSR + R15 10 mΩ | กระแสชาร์จ/คายประจุในสาขาแบตเตอรี่ |
| Charger | U4 BQ25601RTWR | power path, charge control/status/fault |
| Device PD sink | U8 HUSB238A-BB001-QN16R | CC/PD status และ voltage request ที่อนุญาต |
| Device eFuse | U5 TPS259474ARPWR | protection ทาง hardware; ไม่มี GPIO ควบคุมจาก MCU ในแบบนี้ |
| Device TVS | TVS1400DRVR และ USBLC6-2SC6 | hardware protection |
| Main 3.3 V | U12 TPS63020DSJR, L5 1 µH ±20% 3.6 A ตามข้อความในวงจร | เปิด/ปิดด้วยสวิตช์ hardware |
| Switched rails | U2/U18 TPS22918DBVR | เปิดไฟจอ+MAX6675 และ SD+GNSS |
| LED | LED4 XL-4020RGBC-2812B + LED1/2/3 SK6812MINI-E | รวม 4 addressable RGB pixels |
| LED power | Q1 AO3401A P-MOS | สัญญาณ gate เปิดเมื่อ LOW |
| Buzzer | ระบุ 2.7 kHz, Q5 driver | alarm/feedback |
| Door | DOOR_NC, pull-up 1 MΩ + capacitor 47 nF | digital door sensor |
| Reset | SW1 ต่อ EN ลง GND | hardware reset ไม่ใช่ปุ่มสั่งงานในแอป |
| Main switch | SW3 MSK12C02 คุม 3V3_EN | ตัด/เปิดภาค 3.3 V ไม่ใช่ GPIO input |
| Battery connector | CN2: CELL_PLUS / GND | แบต Li-Po 1S ตาม requirement |
| Display panel [REQ ล่าสุด] | Waveshare 2.13inch e-Paper (G), raw display, 250×122, แดง/เหลือง/ดำ/ขาว | driver 4 สีตาม revision ของแผงจริง |
| Display connector | FPC1 FPC-05F-24PH20, 24 contacts + mounting pads | e-paper SPI และวงจร boost |
| Display boost | L7 68 µH, Q3 SI1308EDL, MBR0530, PREVGH/PREVGL | sequence ต้องตรง panel/controller |

ความจำ module อ้างอิง [Espressif module datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)

ข้อจำกัด:

- WROOM-1U ต้องมีเสา Wi-Fi/BLE 2.4 GHz ภายนอกของ module แยกจากเสา GNSS
- LIS2DW12 เป็น accelerometer ไม่มี gyroscope
- ไม่มี humidity sensor, cellular modem, LoRa transceiver, Zigbee/802.15.4 radio, heater, compressor หรือ actuator ควบคุมความเย็นในวงจรนี้
- ESP32-S3 ใช้ BLE; อย่าสร้าง Bluetooth Classic SPP เป็นช่องทางหลัก
- GNSS ให้พิกัด แต่ไม่ได้ส่งข้อมูลออกอินเทอร์เน็ตเอง

### 2.2 Power path และ power domains

[HW] เส้นทางไฟหลัก:

USB-C VBUS -> TVS/eFuse -> VBUS_PROTECTED -> BQ25601 -> SYS_BQ -> TPS63020 -> 3V3_MAIN

BQ25601 BAT -> BAT_CHARGER -> shunt R15 10 mΩ -> CELL_PLUS -> Li-Po

SW3 เลือก SYS_BQ หรือ GND ให้ 3V3_EN ของ TPS63020
จึงตัดไฟ main 3.3 V ได้โดยยังเหลือภาค charger ที่รับ VBUS

| Domain | อุปกรณ์/โหลด | การควบคุม |
|---|---|---|
| 3V3_MAIN | ESP32, LIS2DW12, NFC, RTC VDD, INA226 VS, HUSB238A VDD, buzzer และ logic pull-ups | SW3 / TPS63020 EN |
| 3V3_EPD_TK | e-paper และ MAX6675 | GPIO42 HIGH = ON |
| 3V3_SD_GNSS | microSD และ GNSS VCC | GPIO47 HIGH = ON |
| 3V3_LED | addressable LED 4 ดวง | GPIO48 LOW = ON, HIGH = OFF |
| CELL_PLUS | cell และ MAX17048 CELL/VDD | ไม่ได้ผ่าน load switch ของ peripheral |
| VBAT_3V | RTC VBAT | มี net นี้ แต่ไม่เห็นแบตสำรอง/แหล่งจ่ายของ net ในหน้า PDF |
| GNSS VBAT | ต่อ 3V3_MAIN | ไม่ได้ต่อ CR1220 ใน schematic นี้ |

ข้อกำหนด Firmware จากการแชร์ rail:

- เปิด SD เท่ากับจ่ายไฟ GNSS VCC ด้วย; ไม่มีขาแยกที่เปิด SD อย่างเดียวแต่ตัดไฟ GNSS จริง
- การบันทึกปกติลง Flash ไม่ต้องเปิด SD/GNSS rail; เปิด rail นี้เมื่ออ่าน GNSS หรือใช้งาน SD จริงตาม policy
- เปิด MAX6675 เท่ากับเปิด rail จอด้วย; ห้ามดับไฟระหว่างจอกำลัง refresh
- ต้องมี rail ownership/reference counting หรือ power manager กลาง ไม่ให้ task หนึ่งดับไฟขณะที่อีก task ใช้งาน
- ก่อนตัดไฟ peripheral: รอธุรกรรมจบ, flush/unmount SD ตามกรณี, จัดขา SPI/UART/RESET/CS ให้ไม่จ่ายไฟย้อนเข้า rail ที่ดับ
- อย่าถือว่า CS ต้องเป็น HIGH ค้างเสมอเมื่อ peripheral ไม่มีไฟ; high output อาจ back-power ผ่าน IO
- ตั้ง GPIO hold/สถานะตอน sleep ให้ตรงกับขาที่ไม่ใช่ RTC GPIO และทดสอบจริง
- LIS2DW12 อยู่ 3V3_MAIN จึงยังเฝ้า motion ได้เมื่อ SD/GNSS rail ปิดและ MCU deep sleep
- HARD OFF ต่างจาก deep sleep: เมื่อ SW3 OFF ไม่มี Firmware ทำงาน/ไม่มี MCU wake จาก door หรือ NFC

## 3. GPIO map ที่ต้องใช้

ตารางนี้เป็นหมายเลข ESP32 GPIO ไม่ใช่หมายเลข pad ของ module
ห้ามสลับโดยอ้าง pinout ของ development board

| GPIO | Net ใน schematic | Direction ที่ MCU | หน้าที่/ข้อสังเกต |
|---:|---|---|---|
| 1 | ST25_GPO | Input | NFC event; กำหนด GPO configuration ให้ตรงชนิด output |
| 2 | ACC_INT1 | Input | LIS2DW12 INT1 |
| 4 | POWER_GOOD_N | Input | BQ25601 PG#, LOW = valid power-good ตามเงื่อนไขชิป |
| 5 | PMIC_IRQ_N | Input | IRQ ร่วมจาก BQ25601 INT# และ HUSB238A INT_N |
| 6 | BUZZER | Output/PWM | Q5 buzzer driver, HIGH ทำให้ transistor นำ |
| 7 | DOOR_NC | Input | reed/magnetic door input |
| 8 | EPD_DC | Output | e-paper data/command |
| 9 | EPD_RST | Output | e-paper reset; มี pull-up 10 kΩ ไป rail จอ |
| 10 | SD_CS | Output | microSD CS |
| 11 | MCU_SPI2_MOSI | Output | SPI2 MOSI ไป SD |
| 12 | MCU_SPI2_CLK | Output | SPI2 SCLK ไป SD |
| 13 | MCU_SPI2_MISO | Input | SPI2 MISO จาก SD |
| 14 | MAX6675_CS | Output | CS ของ MAX6675 กำหนดแล้วในรุ่นนี้ |
| 15 | IIC1_SDA | Bidirectional open-drain | I²C shared bus |
| 16 | IIC1_SCL | I²C clock | I²C shared bus |
| 17 | GNSS__RX | UART TX ของ MCU | ไป CN1 RXD0 pin 3 ผ่าน R45 1 kΩ |
| 18 | GNSS_TX | UART RX ของ MCU | รับจาก CN1 TXD0 pin 2 |
| 19 | D- | USB | native USB D- ผ่าน R90/ESD |
| 20 | D+ | USB | native USB D+ ผ่าน R91/ESD |
| 21 | EPD_BUSY | Input | BUSY_N: LOW = busy, HIGH = ready ตามเอกสารจอ G; ตรวจซ้ำบนบอร์ดจริง |
| 38 | EPD_CS | Output | CS ของ e-paper |
| 39 | MCU_SPI3_MISO | Input | SPI3 MISO จาก MAX6675 |
| 40 | MCU_SPI3_CLK | Output | SCLK ร่วม e-paper/MAX6675 |
| 41 | MCU_SPI3_MOSI | Output | MOSI ของ e-paper |
| 42 | EPD_TK_PWR_EN | Output | HIGH เปิด U2 |
| 43 | LED (TXD0 pad) | RMT output | data ไป LED4 ซึ่งเป็นดวงแรก |
| 44 | CHG_CE_N (RXD0 pad) | Output | LOW อนุญาต charger; HIGH ปิด charging |
| 47 | SD_GNSS_PWR_EN | Output | HIGH เปิด U18 |
| 48 | LED_PWR_EN | Output | LOW เปิด P-MOS LED, HIGH ปิด |

ข้อควรระวัง:

- GNSS net ตั้งชื่อจากฝั่ง module: ต้องใช้ MCU TX=GPIO17, MCU RX=GPIO18 ตามการเดินสายใน PDF ห้ามตั้ง RX=17 โดยอ่านชื่อ net อย่างเดียว
- GPIO43/44 ถูกใช้กับ LED/charge enable แล้ว ห้ามเปิด UART0 console บนสองขานี้ใน application
- ROM boot output ที่ TXD0 ต้องประเมินผลกับ LED ใน bring-up; การตั้ง application log ไม่ได้เปลี่ยน ROM boot waveform
- GPIO0 ไม่มีปุ่ม BOOT/การต่อสั่งงานแอปให้เห็นในหน้านี้ อย่าสร้างปุ่ม START/STOP Trip บน GPIO0
- GPIO3/45/46 และ GPIO35/36/37 ไม่ได้ใช้งานในวงจร
- GPIO35/36/37 ของ variant ที่มี Octal PSRAM ห้ามนำกลับมาใช้เป็น GPIO อิสระโดยเดา; ตรวจข้อจำกัด module ก่อน
- EN เป็น hardware reset และ 3V3_EN เป็น hardware switch control ไม่ใช่ output ที่ MCU สั่งตัดตัวเองผ่าน Firmware

### 3.1 I²C

[HW] GPIO15 SDA, GPIO16 SCL, pull-up 4.7 kΩ ไป 3V3_MAIN
[PROPOSED] เริ่ม bring-up ที่ 100 kHz; เมื่อผ่านแล้วจึงใช้ 400 kHz หากทุกอุปกรณ์/ลายสัญญาณรองรับ
ใช้ address แบบ 7-bit ใน driver และมี bus mutex/transaction timeout/recovery

| IC | Address 7-bit | เงื่อนไข |
|---|---|---|
| LIS2DW12 | 0x18 | SA0 ลง GND ตามวงจร; ตรวจ WHO_AM_I |
| MAX17048 | 0x36 | ระบุใน schematic |
| INA226 | 0x40 | A0/A1 ลง GND; ระบุใน schematic |
| HUSB238A | 0x42 | ระบุใน schematic; ตรวจ full datasheet และ I²C mode strap |
| ST25DV04KC user/dynamic/mailbox | 0x53 ค่าโรงงาน | อาจเปลี่ยนได้ด้วย I2C_CFG |
| ST25DV04KC system/config | 0x57 ค่าโรงงาน | ไม่ใช่อุปกรณ์เพิ่มอีกตัว |
| PCF8523 | 0x68 | address มาตรฐาน |
| BQ25601 | 0x6B | address มาตรฐาน |

Address/behavior อ้างอิง [LIS2DW12](https://www.st.com/resource/en/datasheet/lis2dw12.pdf), [ST25DV04KC](https://www.st.com/resource/en/datasheet/st25dv04kc.pdf), [PCF8523](https://www.nxp.com/docs/en/data-sheet/PCF8523.pdf) และ [BQ25601](https://www.ti.com/lit/ds/symlink/bq25601.pdf)

[HW] PMIC_IRQ_N ใช้ร่วมกัน ดังนั้น ISR ต้องแจ้ง event ให้ task อ่าน/จัดการทั้ง BQ25601 และ HUSB238A ไม่ใช่สรุปว่า interrupt มาจาก IC ใดตัวเดียว
ไม่ทำ I²C blocking ใน ISR

### 3.2 SPI

| Bus | Pins | Devices | CS |
|---|---|---|---|
| SPI2 | MOSI11, SCLK12, MISO13 | microSD เท่านั้น | SD_CS10 |
| SPI3 | MOSI41, SCLK40, MISO39 | e-paper + MAX6675 | EPD_CS38, MAX6675_CS14 |

- มี series resistors 22 Ω บนสัญญาณ SPI ที่ออกจาก MCU
- ใช้ per-device SPI configuration/clock และ bus arbitration
- MAX6675 อ่านอย่างเดียว ไม่มี MOSI
- ห้ามเลือก CS ทั้ง e-paper และ MAX6675 พร้อมกัน
- รอจอ BUSY แบบ non-blocking/timeout และไม่ถือ bus mutex ค้างตลอดช่วง refresh โดยไม่จำเป็น
- SD ไม่ใช่ SDMMC 4-bit ใน PCB รุ่นนี้

## 4. Sensor และอุปกรณ์ประกอบ

### 4.1 Temperature / MAX6675

[REQ] ใช้ thermocouple K
[PROPOSED] บันทึก raw reading, calibrated temperature, validity และ fault

ต้องรองรับ:

- power-on settling และรอ conversion ให้ครบก่อนอ่าน
- CS HIGH ระหว่าง conversion; การอ่านต้องไม่ทำให้ conversion ใหม่ไม่เคยเสร็จ
- probe open/disconnected เป็น fault ไม่แปลงเป็น 0 °C หรือถือว่าค่ายังปกติ
- offset/gain calibration แบบมี version/date และยังเก็บ raw value
- timeout, SPI error, invalid/stale sample
- threshold, hysteresis และ dwell time ของ alarm เป็น configuration

ข้อจำกัด: MAX6675 มี resolution 0.25 °C, ช่วงอ่าน thermocouple 0 ถึงประมาณ 1024 °C และ conversion สูงสุด 220 ms ตาม datasheet; resolution ไม่ใช่ accuracy ไม่ให้สัญญาวัดแม่น ±0.25 °C หรือ ±0.5 °C โดยไม่มีผลสอบเทียบ ไม่ใช้ค่าจาก MAX6675 เพื่ออ้างว่าวัดอุณหภูมิติดลบได้ [MAX6675 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX6675.pdf)

[PROPOSED] profile เริ่มต้นสำหรับ cold transport: 2-8 °C เป็นค่า configurable ที่ต้องยืนยันกับการใช้งานจริง ไม่ใช่เกณฑ์ที่เหมาะกับยาทุกชนิด

### 4.2 Door magnetic sensor

[REQ] magnetic contact แบบ Normally Closed และเน้นประหยัดไฟ
[HW] GPIO7, pull-up 1 MΩ ไป 3V3_MAIN, capacitor 47 nF ลง GND

- contact ปิดวงจรลง GND -> LOW
- contact เปิดวงจร -> HIGH
- ต้องตรวจสภาพ contact เมื่อวางแม่เหล็กที่ตำแหน่งฝาปิดจริงก่อน map LOW/HIGH เป็น DOOR_CLOSED/DOOR_OPEN เพราะคำว่า NC ของสินค้าอาจอ้างอิงสภาพไม่มีแม่เหล็ก
- ทำ door polarity เป็น config
- ไม่เปิด internal pull-up ค่าแรงโดยไม่จำเป็น เพราะเพิ่มกระแสและเปลี่ยน RC behavior
- RC nominal ประมาณ 47 ms; debounce ซอฟต์แวร์เพิ่มเติมโดยทดสอบ waveform จริง
- บันทึก open/close event, เวลาเปิดนานเท่าไร, จำนวนครั้งต่อ Trip
- debounce และตรวจซ้ำหลัง wake
- พิจารณา wake เมื่อระดับเปลี่ยนตามสถานะประตูล่าสุด อย่าให้ประตูเปิดค้างทำ MCU ตื่นวน
- contact สองสายนี้แยกสายขาดกับ contact เปิดไม่ได้ ให้รายงาน open/contact fault ตามความสามารถจริง

### 4.3 LIS2DW12

[HW] จ่ายไฟจาก 3V3_MAIN; INT1 -> GPIO2; INT2 ไม่ต่อ
[PROPOSED] low-power motion sensing, wake-on-motion, shock threshold, optional tilt/free-fall

- ใช้ low-power ODR/range ที่เหมาะกับความละเอียดและแรงกระแทกที่ต้องจับ
- บันทึก XYZ, magnitude, threshold source และ overflow/saturation ตามโหมด
- กำหนดชนิด/active level/latch ของ INT1 ให้ตรง wake strategy
- clear source interrupt ก่อนเข้า sleep
- อย่าถือว่า interrupt wake ที่เกิดแล้ว MCU อ่านทีหลังคือค่า peak ขณะกระแทก ใช้ FIFO/event capabilities หรือรายงานว่าเป็น event-only
- ค่า g threshold, duration, full scale และ ODR ยังเป็น [VERIFY]
- ไม่มี gyro และไม่รับประกันทิศทางการหมุนเต็มรูปแบบ
- ไม่ใช้ temperature ภายใน accelerometer เป็นอุณหภูมิสินค้า/แบตโดยไม่มีการประเมิน

### 4.4 GNSS

[HW] ATGM336H-6N-74:

- MCU TX GPIO17 -> GNSS RXD0 pin3 ผ่าน R45 1 kΩ
- MCU RX GPIO18 <- GNSS TXD0 pin2
- VCC = 3V3_SD_GNSS
- VBAT = 3V3_MAIN
- 1PPS, ON/OFF, nRESET ไม่ต่อ MCU

[PROPOSED] UART NMEA parser:

- เริ่มด้วย configurable baud; 9600 8N1 เป็นเพียงค่าทดลอง bring-up ต้องยืนยัน module configuration
- checksum validation
- latitude/longitude, fix valid, fix quality/type, satellite count, HDOP, speed/course และ UTC ตาม sentence ที่ module ส่งจริง
- จำกัดเวลา acquisition/retry, ไม่รอ fix ไม่สิ้นสุด
- เมื่อไม่มี fix ให้ log no-fix และอายุของ last-known position; ห้ามส่งตำแหน่งเก่าเหมือนเป็นตำแหน่งปัจจุบัน
- เปิด GNSS เป็นช่วงและพิจารณา motion-aware acquisition
- UART standby/config command ใช้ได้เมื่อมี protocol ของ module ยืนยันเท่านั้น
- ไม่สมมติว่ามี hardware reset/1PPS interrupt ให้ใช้
- ปิด SD/GNSS rail แล้ว GNSS backup ยังอาจคงไว้ผ่าน 3V3_MAIN; SW3 OFF จะตัด backup นี้ด้วย
- เสาอากาศเลือกด้วย parts ที่ populate ไม่ใช่ software switch

### 4.5 RTC / เวลา

[HW] PCF8523 address0x68, crystal32.768 kHz; note ในแบบให้ Control_1 register0x00: CAP_SEL=1

- ใช้ read-modify-write เพื่อไม่ทับ control bits อื่น และตรวจ crystal load specification
- ตรวจ oscillator-stop/battery status
- รองรับตั้งเวลาจากแอป/PC หรือ GNSS UTC ที่ตรวจ validity แล้ว; NTP เป็น optional เมื่อมี Wi-Fi
- เก็บ UTC ในข้อมูล และแปลง timezone ที่แอป/display
- มี monotonic sequence/tick สำหรับเรียง event แม้เวลาจริงถูกแก้ย้อนหลัง
- เก็บ time_quality เช่น VALID_RTC, GNSS_SYNCED, HOST_SYNCED, UNKNOWN
- INT1#/CLKOUT ไม่ได้ต่อ MCU จึงไม่ใช้ PCF8523 alarm เป็นขา wake ในรุ่นนี้
- ใช้ ESP32 internal RTC timer สำหรับ periodic wake
- net VBAT_3V ต้องตรวจแหล่งจ่ายจริงก่อนอ้างว่า SW3 OFF แล้วยังเก็บเวลาได้

### 4.6 MAX17048 และ INA226

[HW] MAX17048 ต่อกับ CELL_PLUS; ALRT# และ QSTRT ไม่ต่อ MCU
[PROPOSED] อ่าน SOC/voltage แบบ polling, ตรวจค่าผิดปกติ, ใช้ low-power configuration ตาม datasheet และไม่ quick-start/reset model ทุก boot โดยไร้เหตุผล [MAX17048 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/MAX17048-MAX17049.pdf)

[HW] INA226:

- A0/A1 = GND -> 0x40
- R15 = 10 mΩ = 0.010 Ω
- VIN+ ผ่าน R12 จาก BAT_CHARGER
- VIN- ผ่าน R13 จาก CELL_PLUS
- VBUS sense ต่อ CELL_PLUS
- ALERT ไม่ต่อ MCU

[PROPOSED] convention: battery_current_mA เป็นบวกเมื่อกระแสจาก charger เข้าสู่ cell และลบเมื่อ cell จ่ายออกผ่าน shunt; ยืนยันทิศด้วยการวัดจริง

- ตั้ง calibration จาก shunt 0.010 Ω และ current LSB ที่เลือกจริง
- ใช้ triggered measurement แล้ว power-down เมื่อไม่ต้องวัดต่อเนื่อง [INA226 datasheet](https://www.ti.com/lit/ds/symlink/ina226.pdf)
- อ่าน signed current ถูกต้อง
- VBUS ของ INA226 ในวงจรนี้หมายถึงแรงดัน CELL_PLUS ไม่ใช่ USB VBUS
- ค่ากระแสนี้ไม่ใช่ USB input current และไม่ใช่กระแสโหลดทั้งหมดตอน adapter จ่ายผ่าน SYS_BQ
- อุปกรณ์ที่ต่อ CELL_PLUS โดยตรงอาจกินกระแสนอกเส้น shunt จึงไม่ใช้ INA226 เพียงอย่างเดียวพิสูจน์ runtime ของทั้งระบบ
- SOC ใช้ fuel-gauge เป็นแหล่งหลัก; ไม่เดา SOC จาก INA226 current snapshot

### 4.7 E-paper: Waveshare 2.13inch e-Paper (G), 4 สี

[REQ ล่าสุดจากเจ้าของ] ใช้ **Waveshare 2.13inch e-Paper (G) raw display, 250×122 pixels, Red/Yellow/Black/White**
นี่คือแผงจอ raw ที่ใช้วงจรขับบน Foam PCB ไม่ใช่บอร์ด HAT สำเร็จรูป
รุ่นจอ ขนาด ความละเอียด และ 4 สีได้รับการยืนยันแล้ว; สิ่งที่ยังต้องตรวจคือ revision ของแผงที่ซื้อและความเข้ากันได้ของวงจรขับ

| รายการ | ข้อกำหนด |
|---|---|
| Visible resolution | 250×122 pixels; application layout แบบ landscape เป็นข้อเสนอ |
| Palette | BLACK / WHITE / YELLOW / RED รวม 4 สี ไม่มีสีเขียวหรือ grayscale จริง |
| Connector / interface | FPC 24 pins, pitch 0.5 mm; 4-wire SPI ตาม BS และวงจรจริง |
| SPI / control | MOSI41, SCLK40, CS38, DC8, RST9, BUSY21 |
| Reset / busy | RST_N active LOW; BUSY_N LOW = busy, HIGH = ready |
| Supply | ใช้ 3V3_EPD_TK, GPIO42 HIGH เปิดไฟ; แชร์ rail กับ MAX6675 |
| Raw panel supply | VDD/VDDIO 2.3–3.6 V ตามเอกสาร; อย่าใช้ช่วงไฟเข้าของ HAT มาอ้างกับ raw panel |
| Data format | 2 bits/pixel, 4 pixels/byte ตาม command table; color code, scan direction, stride และ padding ต้องตรง driver/revision |
| Image persistence | ภาพค้างได้เมื่อไฟดับ จึงต้องมีเวลาที่อัปเดตล่าสุดและไม่ใช้ภาพค้างเป็นหลักฐานว่า MCU ยังทำงาน |

เอกสารผู้ผลิตมีอย่างน้อยสองชุด:

- [2.13inch e-Paper (G) Specification รุ่นเดิม](https://files.waveshare.com/upload/5/57/2.13inch_e-Paper_%28G%29_Specification.pdf): revision 1.0 วันที่ 2023-05-21, typical image update 25 s ที่ 25 °C
- [2.13inch e-Paper (G) V2 User Manual](https://files.waveshare.com/wiki/2.13inch_NFC_Powered_EPaper_G/2.13inch_e-Paper_%28G%29_V2.pdf): document revision 1.0 วันที่ 2025-05-15, typical full/fast update 16/11 s ที่ 25 °C

[VERIFY] ชื่อที่เจ้าของให้ยังไม่ระบุ V2 จึงห้ามเลือก V2 หรือใช้ตัวเลขเวลา refresh ของ V2 เป็นสเปกของแผงจริงโดยอัตโนมัติ
ให้ตรวจ SKU/marking บน FPC/เอกสารจากผู้ขาย แล้วใช้ official example/init sequence/LUT ของ revision นั้น
ยังไม่ล็อกชื่อ controller IC จากการคาดเดา; ห้ามนำ driver จอ 2 สี/3 สี หรือ SSD1680/UC8151 รุ่นใกล้เคียงมาแทนเพียงเพราะขนาด 2.13 นิ้วเหมือนกัน

ข้อกำหนด driver:

- แยก display HAL, panel-specific driver และ application view model; เตรียมกราฟิก 250×122 และ palette 4 สีได้ทันที
- ใช้ full refresh เป็น baseline; เปิด fast/partial refresh เฉพาะ mode ที่ผู้ผลิตรองรับใน revision จริงและทดสอบแล้ว การมี command partial window ไม่ใช่หลักฐานรับรอง partial refresh ของแผง
- ตรวจ RAM geometry ของ controller แยกจาก visible geometry; command table V2 มีค่าเริ่มต้น 128×250 จึงห้ามใช้ 250×122×2/8 = 7,625 bytes เป็น SPI payload โดยไม่ตรวจ padding/scan mapping
- enum สีระดับแอปต้อง map ไปค่า 2-bit ตาม official driver และทดสอบด้วยแถบสีทั้งสี่; ห้ามเดา byte mapping จากชื่อสี
- power-on/reset/init/write frame/power-on boost/refresh/power-off boost/deep-sleep ตาม sequence ของ revision จริง; รอ BUSY_N กลับ HIGH ในขั้นที่เอกสารกำหนด
- มี BUSY timeout ที่ยาวพอกับอุณหภูมิและ waveform, recovery แบบจำกัดครั้ง และไม่ค้าง task อื่นตลอดการ refresh
- refresh เฉพาะเมื่อข้อมูลเปลี่ยนและครบ minimum interval ที่ตรวจแล้ว; รวมหลายการเปลี่ยนแปลงเป็นหนึ่ง frame
- หลัง refresh ให้ panel เข้า sleep ตาม driver; ปิด shared rail ได้ต่อเมื่อ MAX6675 และ panel ปล่อยสิทธิ์ใช้งานแล้ว
- เมื่อเปิด rail เพียงเพื่ออ่าน MAX6675 อย่าสั่งจอ refresh ทุกครั้ง; ต้องวัดกระแสและตรวจสภาพ panel/reset ระหว่าง rail cycling
- ทั้งสองเอกสารระบุ operating temperature 0 ถึง +40 °C ในตาราง absolute maximum; ตัวเลขนี้ไม่ได้รับรองความเร็วหรือคุณภาพ refresh ที่ 2–8 °C ให้ทดสอบอุณหภูมิที่ตัวจอจริง และหลีกเลี่ยง refresh นอกเงื่อนไขของผู้ผลิต
- refresh failure ต้องส่ง health fault พร้อม timestamp; sensor logging และ Trip ทำงานต่อได้

[VERIFY — วงจร raw panel] ก่อนเปิด boost ต้องตรวจ schematic/PCB กับ reference circuit ของ revision ที่ซื้อ:

| จุดตรวจ | Foam schematic | เอกสาร Waveshare ที่ตรวจ | งานที่ต้องทำ |
|---|---|---|---|
| FPC pin 4 | ต่อ C62 1 µF ลง GND | NC / keep open ทั้งรุ่นเดิมและ V2 | ยืนยัน pinout/footprint และแก้ตามแผงจริง |
| FPC pins 6/7 | ไม่ต่อ (NC) | TSCL/TSDA; pin table ระบุให้ดึง HIGH หรือ LOW เมื่อไม่ใช้ แต่ reference drawing ไม่แสดง resistor | ขอข้อกำหนดที่ชัดจากผู้ผลิต/ตรวจ official board; ไม่ใช้ GPIO ที่ไม่มีสายจริงมาแก้ |
| Boost inductor | L7 68 µH | reference 47 µH / 500 mA | ตรวจวงจร/กระแสอิ่มตัว/แรงดันกับผู้ผลิตก่อนยืนยันว่าเข้ากันได้ |
| Boost sense resistor | R33 0.470 Ω | reference 2.2 Ω | ตรวจผลต่อ boost current limit; ไม่แก้ด้วย waveform ที่เดาขึ้น |
| Gate resistor to GND | R44 10 kΩ | reference 1 MΩ | ตรวจร่วมกับ boost network ทั้งชุด |

ความต่างจาก reference ไม่ได้พิสูจน์ว่าบอร์ดใช้ไม่ได้ แต่ยังห้ามอ้างว่ารองรับแผงนี้ผ่านแล้ว และต้องตรวจ voltage rating ของ capacitors/diodes และด้านสัมผัสของ FPC ด้วย

[PROPOSED] layout 250×122:

- ตัวเลขอุณหภูมิขนาดใหญ่, หน่วย °C และสถานะ valid/probe fault
- Device ID/Trip state, SOC/charging, door และเวลาที่อัปเดตล่าสุด
- พื้นหลังขาว/ข้อความดำ; เหลืองสำหรับ warning; แดงสำหรับ alarm; มีข้อความหรือ icon ควบคู่สีเสมอ
- รายละเอียด min/max, Trip ID, link/fix status อยู่หน้ารองหรือย่อส่วนตามพื้นที่จริง ไม่ยัดทุก field ลงหน้าเดียว
- เริ่มด้วย English/ตัวเลข; หากต้องใช้ภาษาไทยให้ระบุ font/license และทดสอบการจัดวางวรรณยุกต์
- หาก revision/วงจรขับยังไม่ผ่าน ให้ปิด physical display driver ด้วย feature flag แต่ให้ view model และ data logging ทำงานต่อ

### 4.8 LED และ buzzer

[HW] chain order:
index0 = LED4 XL-4020RGBC-2812B
index1 = LED1 SK6812MINI-E
index2 = LED2 SK6812MINI-E
index3 = LED3 SK6812MINI-E

Data GPIO43, power GPIO48 active LOW

- ใช้ RMT หรือ driver ที่รักษา timing
- ตรวจ RGB/GRB order และ timing ของ LED สอง part; อาจต้องมี pixel-specific color-order mapping
- brightness cap, short indication, ปิด rail เมื่อไม่ใช้
- reset data ให้อยู่ในสภาพไม่ back-power ตอน rail ปิด
- ไม่มี Dock LED ในรุ่นนี้

[PROPOSED] status patterns configurable:
boot/self-test, Trip active, BLE session, USB transfer, charging/full, warning/fault
ไม่ให้ LED ติดสว่างค้างตลอดการขนส่ง

Buzzer GPIO6 ขับ Q5:

- ค่าในแบบ 2.7 kHz แต่ยังต้องยืนยัน active/passive และ part จริง
- หาก passive ใช้ PWM; หาก active ใช้ on/off ตามสเปก
- pattern ไม่ blocking, duration/duty bounded, mute/ack alarm ได้
- Alarm acknowledgement ไม่ลบประวัติ alarm

## 5. Charging และ USB-C policy

### 5.1 แยกค่าที่ห้ามใช้ปนกัน

- ICHG = กระแสเข้าก้อนแบตเตอรี่
- IINDPM = กระแสรวมที่ charger อนุญาตดึงจาก input
- CC advertised current = ความสามารถที่ source แจ้ง
- PD contract = voltage/current ที่ตกลงกับ source
- Dock slot hardware current limit = ค่าที่วงจร SY6280 กำหนด

[REQ] นอก Dock เป้าหมาย ICHG 0.5 A; Dock เป้าหมาย ICHG 1.2 A
[VERIFY] ตัวเลขนี้ยังต้องไม่เกินความสามารถเซลล์ แหล่งจ่าย และ input budget

BQ25601 ตั้ง ICHG เป็นขั้น 60 mA ดังนั้น target ไม่เกิน 500 mA ใช้ 480 mA; 1200 mA ตั้งได้ตรง
IINDPM ต้องกำหนดแยกและเผื่อกำลังของระบบ ไม่ถือว่า ICHG=IINDPM
อ้างอิง [BQ25601 datasheet](https://www.ti.com/lit/ds/symlink/bq25601.pdf)

### 5.2 ตารางนโยบาย

| สถานการณ์ | พฤติกรรมเป้าหมาย |
|---|---|
| แบตอย่างเดียว | ไม่ชาร์จ, ปิด OTG/boost, ทำงานประหยัดไฟ |
| Unknown source / เริ่ม attach | ใช้ conservative current; ยังไม่เปิด high-charge profile |
| USB host แบบ legacy ก่อน enumeration | คุม input ตาม USB allowance; application ต้องไม่ถือว่าดึง 500 mA ได้ทันทีทุกกรณี |
| General adapter/Notebook ที่ยืนยันความสามารถแล้ว | ICHG ไม่เกิน 480 mA ตามนโยบายล่าสุด |
| Verified Foam Dock | ICHG เป้าหมาย 1200 mA แต่ลดได้เมื่อ input/DPM/thermal/cell limit บังคับ |
| Rp แจ้ง 1.5A แต่ยังไม่รู้ว่าเป็น Dock | เป็นแหล่ง 1.5A ที่ยังไม่ทราบตัวตน; default conservative/non-Dock profile |
| Device ต่อ PD adapter โดยตรง | 5 V baseline; 9 V เป็น conditional feature หลังตรวจ hardware/default/นโยบาย |
| Battery fault/charge fault | ลด/หยุด charging ตามชนิด fault และยังคง data logging ถ้าพลังงานพอ |
| SOC ถึง 80% | ตามแนวคิดเดิมให้ลดความเร็วได้ แต่ threshold/hysteresis/slow current ยังต้องกำหนด |
| SW3 OFF | MCU ไม่ทำงาน; การชาร์จขึ้นกับ hardware/autonomous behavior ไม่ใช่ Firmware ใหม่ |

[REQ] เคยต้องการ fast charge 0–80% แล้วช้าลง
[PROPOSED] แยกเป็น product policy เหนือ CC/CV ของชิป; ไม่ปิดการป้องกัน/termination และไม่บังคับ full เพียงเพราะ SOC แตะ 100%
[VERIFY] การใช้ PD 9 V กับ adapter ทั่วไปต้องไม่กลายเป็นอนุญาตกระแสชาร์จเกินนโยบาย non-Dock 0.5 A โดยอัตโนมัติ

### 5.3 ข้อจำกัดการระบุ Dock

[HW] Dock downstream CC1/CC2 ใช้ Rp 22 kΩ ไป 5V_CHx
ค่า Rp บอก Type-C current capability ไม่ใช่ serial number/ชนิดสินค้าหรือเครื่องหมายเฉพาะ Foam

ดังนั้นห้ามเขียน:

- Rp 1.5 A -> is_foam_dock=true
- VBUS 5 V -> is_foam_dock=true
- USB enumerated -> is_foam_dock=true

[HW] Dock ไม่มี MCU, ไม่มีสาย Dock ID ถึง MCU Device และไม่มี downstream PD source controller
Device ไม่สามารถรู้ upstream hub topology จากบทบาท USB peripheral ของตัวเองเหมือนที่ PC host เห็น

[PROPOSED] ทำ source_class และ dock_identity เป็นคนละ field

- source_class เช่น UNKNOWN, USB_DEFAULT, TYPEC_1A5, TYPEC_3A, PD_CONTRACT
- dock_identity เช่น UNKNOWN, HOST_VERIFIED
- PC companion อาจตรวจ hub topology แล้วส่ง session-scoped dock authorization/slot info ให้ Device
- token/authorization ต้องหมดเมื่อ detach และห้ามให้คำสั่งข้าม physical current limit
- วิธีนี้ใช้ไม่ได้เมื่อชาร์จ Dock โดยไม่มี PC; strict Dock-only fast charge แบบ standalone ยังเป็น [VERIFY]
- หากต้องการ standalone ให้เจ้าของตัดสินใจ hardware identity เพิ่ม หรือเปลี่ยนนโยบายให้แหล่ง Type-C ที่ผ่านเงื่อนไขทุกตัวใช้ high-current ได้
- ระหว่างยังไม่ตัดสินใจ ให้พัฒนา policy interface และใช้ conservative default; อย่าเงียบแล้วเลือกนิยาม Dock ใหม่เอง

### 5.4 BQ25601 ที่ต้องใส่ใน Firmware

- CHG_CE_N GPIO44 active LOW; ในวงจรมี R22 pull-down 10 kΩ จึงไม่ได้ default-disable charging เมื่อ MCU reset/OFF
- PSEL ถูกดึงจาก REGN ผ่าน R8; ห้ามถือว่า hardware cold-start บน legacy USB ได้รับการคุม 100 mA จาก Firmware ก่อน MCU boot
- ระหว่าง init ให้หยุด/จำกัด charging จนอ่านและตั้งค่าที่จำเป็นสำเร็จ
- program/read-back limits, อ่าน status/fault และจัดการ PMIC_IRQ_N ร่วม
- ค่าชาร์จ, cell voltage limit, precharge/termination และ safety timing ต้องสัมพันธ์กับ datasheet ของแบตจริง
- watchdog ของ charger ต้องมี strategy: service ระหว่างตื่น หรือ configure ให้เหมาะกับช่วงหลับ; ห้าม sleep 300 s แล้วปล่อย watchdog ทำค่ากลับ default อย่างไม่รู้ตัว
- ถ้าเข้า PD 9 V ต้องตั้ง charger input OVP ให้รองรับก่อนส่งคำขอแรงดัน และจัดลำดับกลับ 5 V อย่างปลอดภัย
- disable OTG/backfeed; ไม่ใช้ ship mode โดยไม่ตรวจวิธี wake เนื่องจาก QON ไม่ต่อ MCU

[HW] TS ใช้ตัวต้านทาน 10k/10k คงที่ ไม่เห็น NTC ติดเซลล์
จึงห้ามรายงานว่ามีการวัด/ป้องกันอุณหภูมิแบตจริง และห้ามเอา thermocouple วัดสินค้าไปแทน cell temperature โดยอัตโนมัติ
[VERIFY] ต้องยืนยันพิกัดชาร์จแบตและการป้องกันอุณหภูมิ โดยเฉพาะงานแวดล้อมเย็น

### 5.5 HUSB238A / PD

[HW] part ใน PDF = HUSB238A-BB001-QN16R ไม่ใช่ BB004 ที่เคยพูดถึง

- CC1/CC2 ต่อ USB-C
- I²C 0x42 ตาม note
- INT_N แชร์ GPIO5
- D+/D-ของ HUSB238A ไม่ต่อ; USB data ไป ESP32 โดยตรง
- GATE ไม่ได้คุม external VBUS switch ในวงจรนี้

[VERIFY] ต้องมี full datasheet/register map สำหรับ HUSB238A และ suffix จริงก่อนเขียน register driver
เอกสาร public ที่ตรวจได้จากผู้ผลิตเป็น product brief 1 หน้า จึงยังไม่ยืนยัน register map และ default-PDO behavior ของ BB001
อย่าใช้ register map ของ HUSB238 ที่ไม่มี A และอย่ารับประกันว่าปิด SW แล้ว BB001 จะขอ 5 V ตามนโยบายโดยอัตโนมัติ
อ้างอิง [Hynetek HUSB238A](https://www.hynetek.com/2643.html)

[PROPOSED] allowlist สำหรับ Device เฉพาะ 5 V/9 V ที่ผ่านการยืนยัน; ห้ามขอ 12/15/20/28/48 V

- ต้องอ่าน source capability และยืนยัน contract สำเร็จ ไม่ตัดสินจากค่าที่เขียนไปอย่างเดียว
- PD ไม่ได้แทน USB data และ D+/D-ไม่ใช้ BC1.2/QC ในวงจรนี้
- ห้ามเรียก QC negotiation ผ่าน ESP USB pins
- รองรับ attach/detach/timeout/fallback
- ปิด 9 V feature จนกว่าจะตรวจ autonomous startup/reset behavior และวงจร protection ครบ
- Dock ช่อง Device จ่าย 5 V เท่านั้น ไม่รองรับการขอ 9 V ที่ช่องนั้น

### 5.6 VBUS detection

[HW] GPIO4 เป็น BQ PG# ไม่ใช่ comparator ชื่อ VBUS_DET_N
ไม่มีเส้น ADC วัด USB VBUS โดยตรงให้เห็น
อย่าใส่ GPIO VBUS_DET_N ที่ไม่ได้มีในบอร์ด และอย่าตีความ PG# ว่าเป็น threshold 4.54 V คงที่

[VERIFY] การใช้ PG#/HUSB status เพื่อจัดการ self-powered USB attach/detach ต้องทดสอบ polarity, response และความถูกต้องกับ USB stack
ไม่ใส่ GPIO4 เป็น active-high VBUS monitor โดยตรงโดยไม่จัดการ inversion และความหมายของ PG#
อ้างอิงข้อกำหนด VBUS monitoring และ shared USB PHY จาก [Espressif USB Device Stack](https://docs.espressif.com/projects/esp-usb/en/latest/esp32s3/usb_device.html)

## 6. Hardware ฝั่ง Dock รุ่นปัจจุบัน

[REQ] Dock ECO 6 ช่อง ไม่มี ESP/MCU ไม่มี LED เน้นต้นทุนต่ำ ใช้ adapter PD ภายนอก เป้าหมาย 45 W
[HW] ไม่มี Firmware ของ Dock เอง; Firmware ที่ต้องเขียนอยู่ใน Device และ software PC เป็นงานอีกส่วน

| Block | Part / net [HW] | หมายเหตุ |
|---|---|---|
| PD input | USB9 | D+/D-ไม่ต่อใน PDF นี้ เป็น power input |
| PD sink | U2 HUSB237-AA001-DN06R, VSET capacitor 20 nF | hardware ขอแรงดัน; target 20 V ต้องตรวจ setting กับ datasheet |
| Input protection | TVS2200DRVR + U5 TPS259474ARPWR | 20 V ->20V_PROTECTED |
| Central buck | U6 TPS51397ARJER | 20V_PROTECTED ->5V_POWER |
| Buck IC capability | TPS51397A class 10 A | ไม่เท่ากับพิกัดทั้ง PCB/inductor/adapter ที่รับรองแล้ว |
| Buck inductor | U13 | ใน PDF ไม่แสดง value/part ต้องยืนยันก่อนอ้าง full-load |
| Data upstream | USB5 | D+/D-เข้า USB hub; CC แต่ละเส้น Rd 5.1 kΩ; VBUS จาก PC |
| Input power mux | U12 TPS2116DRLR | VIN1=5V_POWER, VIN2=VBUS, OUT=5V_SYS |
| Hubs | U1+U7 FE1.1S-BQFN24B | cascade 2 ตัวเพื่อให้ 6 ช่องใช้งาน |
| Hub clocks | crystal 12 MHz สองชุด | hardware |
| Slot switches | U3/U4/U8/U9/U10/U11 SY6280AAAC | ISET resistor 3.4 kΩต่อช่อง; EN ผูก 5V_POWER |
| Slot CC | Rp 22 kΩต่อ CC1/CC2 ทุกช่อง | ประกาศ current capability; ไม่ใช่ unique Dock ID |
| Data ESD | USBLC6-2SC6 | upstream/downstream |
| 5 V input protection | TVS0500DRVR | ฝั่ง USB5 |

TPS51397A เป็น IC 10 A ตาม [TI datasheet](https://www.ti.com/lit/ds/symlink/tps51397a.pdf)
TPS2116 เป็น power mux พิกัด 2.5 A ตาม [TI datasheet](https://www.ti.com/lit/ds/symlink/tps2116.pdf) จึงไม่ใช้เป็นทางรวมจ่าย 6 ช่อง 7.2 A

### 6.1 ข้อสรุปจาก net ที่มีจริง

- USB9 รับ PD power; ไม่ใช่พอร์ต data+charge ในไฟล์นี้
- USB5 เป็น data upstream
- ไม่มี USB data mux สำหรับสลับสอง host และไม่ได้ต่อ D+/D-ทั้งสอง input ร่วมกัน
- TPS2116 ยังมีอยู่ใน PDF ล่าสุด
- พบ net 5V_SYS ที่ power mux แต่ hub VDD5 และ slot switch ยังรับ 5V_POWER
- จากการไล่ net ในหน้านี้ ไม่เห็น 5V_SYS ไปเลี้ยงโหลด hub ดังกล่าว จึงยังรับประกันไม่ได้ว่าเสียบ USB5 จาก PC อย่างเดียวแล้ว Dock จะ enumerate/ดึงข้อมูลได้
- ต้องตรวจ/แก้ net เรื่อง data-only operation ใน hardware ถ้ายังเป็น requirement
- Device firmware แก้ net นี้ไม่ได้
- SY6280 เปิดด้วย hardware ไม่มี MCU สั่งตัดไฟ/อ่าน fault แยก slot
- R_ISET 3.4 kΩเป็นค่าที่เห็นจริง แต่ไม่ยืนยันว่าเป็น trip limit 1.2 A; ต้องคำนวณจาก datasheet/tolerance ของ SY6280
- ไม่มี FUSB302/TCA9548A/ESP32-C3 ในรุ่นนี้ และไม่มี SBU protocol ใช้งานจริงถึง Device MCU

### 6.2 Connector และ hub port mapping

| Slot | Connector | Hub downstream port path |
|---|---|---|
| CH1 | USB1 | U1 port3 |
| CH2 | USB10 | U1 port4 |
| CH3 | USB11 | U1 port2 |
| CH4 | USB2 | U1 port1 ->U7 port3 |
| CH5 | USB3 | U1 port1 ->U7 port4 |
| CH6 | USB4 | U1 port1 ->U7 port2 |

U7 port 1 ไม่ได้ใช้
PC companion ใช้ topology ร่วมกับ serial/device ID เพื่อแสดงตำแหน่ง slot; COM port number อย่างเดียวไม่ใช่ตัวตนถาวร
เลข port ในตารางมาจาก DM1/DP1...DM4/DP4 ใน symbol ต้องยืนยันกับ enumeration บน prototype

### 6.3 Power budget

- เป้าหมาย 45 W เป็น adapter input ไม่ใช่ 45 W ที่เหลือจ่าย slot ทั้งหมดหลัง conversion
- หากจำกัด input ของแต่ละ Device ที่ 5 V × 1.2 A: 6 ช่องรวม 36 W ก่อนคิด hub และ loss ของ buck
- สมมติ buck efficiency 90% จะต้องใช้ประมาณ 40 W สำหรับ slot 36 W แล้วยังต้องเผื่อ hub/ส่วนอื่น
- ถ้าทุกช่องดึงตาม advertisement 1.5 A พร้อมกันจะเป็น 45 W ที่ slot ก่อน loss จึงไม่ควรอ้างว่า adapter 45 W พอรองรับ 6 × 1.5 A ต่อเนื่อง
- ICHG 1.2 A เข้าก้อนแบตไม่ใช่ 5 V input 1.2 A ตรงตัว
- เพราะ Dock ไม่มี MCU จึงไม่มี distributed dynamic power allocation ให้เอง
- [PROPOSED] ใช้ static per-device input budget ที่ทดสอบแล้ว และให้ DPM ลด charge current เมื่อระบบใช้ไฟมาก
- ไม่ถือว่าคำสั่ง software เพิ่ม current สามารถเพิ่มพิกัด adapter/buck/switch/สายได้

## 7. Functional scope ของ Firmware

ตารางนี้แยกสิ่งที่ต้องการเดิมกับข้อเสนอสำหรับทำ Firmware ครบระบบ

| Function | ขอบเขต | สถานะ |
|---|---|---|
| Device identity | Device ID คงที่, model, hardware/firmware version | PROPOSED รองรับ REQ เรื่อง NFC/แอป |
| Periodic wake / online send | offline baseline 300 s; online ส่ง sample/event ใหม่ทันทีและระบาย backlog | REQ |
| Battery runtime | เป้าหมาย 7 วันจาก 1500 mAh | REQ; ต้องวัด |
| Temperature logging | temp/raw/calibration/fault | PROPOSED จาก sensor ที่มี |
| Door monitoring | open/close/duration/count | REQ ใช้ NC + PROPOSED รายละเอียด log |
| Motion/shock | event, XYZ/peak เมื่อ hardware รองรับ | PROPOSED |
| GNSS tracking | fix/last-fix age/time/position | PROPOSED |
| Primary local storage | Flash log หลัก, custom partition, offline capacity 31 วัน | REQ; layout/record format เป็น PROPOSED |
| Optional SD | backup/overflow ทริปเก่า; ไม่มีการ์ดก็ทำงานครบ | REQ |
| Retention | server ACK แล้วคืนพื้นที่; พื้นที่เต็มจัดการเก่าก่อนทีละ Trip | REQ |
| Trip workflow | เริ่ม Trip ด้วย NFC+แอป/BLE; stop/report | REQ เริ่ม Trip + PROPOSED รายละเอียด |
| NFC | ID/NDEF/link/app handoff/GPO wake | REQ แนวทาง + PROPOSED protocol |
| BLE | iOS status/config/control/download | REQ ช่องทาง + PROPOSED services |
| Wi-Fi | live upload ระหว่าง Trip และ incremental backlog upload เมื่อกลับมา online | REQ; server API/ACK ยัง VERIFY |
| USB MSC / CSV | PC/NB เห็น Flash Drive ที่มี CSV แยก Trip แม้ไม่มี SD | REQ; read-only snapshot เป็น PROPOSED |
| USB control | optional CDC/vendor commands ร่วมกับ MSC | PROPOSED |
| Dock 6 เครื่อง | PC คุย Device 6 ตัวผ่าน hub | REQ |
| Battery/charge | SOC/voltage/current/charge state | REQ แนวทาง + PROPOSED UI |
| Alarm | temperature/door/shock/low-battery/sensor fault | PROPOSED; thresholds ยัง VERIFY |
| E-paper | Waveshare G 2.13 นิ้ว 250×122, 4 สี; status/Trip/temperature/battery | REQ รุ่นจอ + PROPOSED layout; VERIFY revision/วงจรขับ |
| LEDs/buzzer | indication/mute | HW รองรับ; patterns PROPOSED |
| OTA/maintenance | controlled update, rollback, diagnostics | PROPOSED |
| Hospital gateway/custom radio | เตรียม transport abstraction | Future/VERIFY; ไม่เพิ่ม LoRa ในบอร์ดนี้ |

## 8. Trip และ state machine

[PROPOSED] อย่าใช้ state enum เดียวที่บังคับ TRIP กับ CHARGING อยู่พร้อมกันไม่ได้
แยกอย่างน้อย:

- operational state: BOOT, SELF_TEST, IDLE, TRIP_ACTIVE, MAINTENANCE, RECOVERY
- power/source state: BATTERY, EXTERNAL, CHARGING, CHARGE_DONE, CHARGE_FAULT
- connectivity state: WIFI_OFFLINE, WIFI_CONNECTED_SERVER_UNAVAILABLE, SERVER_ONLINE และ BLE/USB session status
- storage state: NORMAL, SPILL_TO_SD, RECLAIMING, FULL; SD_ABSENT เป็นสถานะปกติของอุปกรณ์ที่ไม่ใส่การ์ด
- USB export state: DETACHED, BUILDING_SNAPSHOT, MOUNTED_READ_ONLY, EJECTED
- alarm flags: หลาย alarm เกิดร่วมกันได้

Trip workflow:

1. แอปอ่าน NFC เพื่อรู้ Device ID และข้อมูลที่ใช้ค้นหา BLE device
2. ผู้ใช้เชื่อม/ยืนยัน session
3. ส่ง Trip metadata/config และ START_TRIP
4. Device ตรวจ Flash/sensor/time/battery แล้วสร้าง Trip ID และ Trip header ที่กู้คืนได้; SD ไม่ใช่เงื่อนไขผ่าน/ไม่ผ่าน START_TRIP
5. ทุก sample/event commit ลง Flash ก่อน; ถ้า server online ให้ส่งทันที ถ้า offline ให้ค้างส่งไว้ใน Flash
6. alarm/event ไม่ทำให้ Trip หยุดโดยไม่ตั้งใจ
7. ดาวน์โหลดระหว่าง Trip ได้ด้วย snapshot/record sequence
8. STOP_TRIP ปิด session และบันทึก summary
9. ปิด Trip metadata และทยอยส่งไป server ผ่าน Wi-Fi; BLE และ USB CSV ใช้ดู/คัดลอกข้อมูลที่ยังมีอยู่ได้โดยอิสระ

อย่าเริ่ม Trip ทุกครั้งที่มี RF field หรือ NFC scan โดยอัตโนมัติ
START/STOP ต้องมี command validation และ idempotency เพื่อกันการส่งซ้ำ
การเสียบ Dock ไม่ควร STOP_TRIP เอง เว้นแต่ตั้ง policy ไว้อย่างชัดเจน
เมื่อ reset กลาง Trip ให้กู้ state และ log เหตุการณ์ restart/data gap ไม่สร้าง Trip ใหม่เงียบๆ

ข้อมูล Trip ขั้นต่ำที่เสนอ:
trip_id, device_id, start/end UTC, config/calibration version, optional operator/container/payload ref, initial battery, alarm counts, door-open time, min/max valid temperature, server_ack status, local storage location (Flash/SD), local completeness, snapshot/export status
ข้อมูลบุคคล/ผู้ป่วยไม่ใช่ข้อมูลจำเป็นใน Firmware นี้

## 9. Flash หลัก, SD เสริม และการส่งข้อมูลไป server

### 9.1 พฤติกรรมที่ยืนยันแล้ว

[REQ ล่าสุด] ใช้ Flash ใน ESP32 เป็น persistent storage หลัก; SD card เป็น optional เท่านั้น
รองรับข้อมูลออฟไลน์อย่างน้อย 1 เดือน โดยใช้ 31 วันในการคำนวณและทดสอบตาม profile ที่ระบุในหัวข้อ 9.3
เป้าหมายเก็บ 31 วันเป็นความจุข้อมูล ไม่ใช่คำยืนยันว่าแบตเตอรี่จะทำงานได้ 31 วัน

| สถานการณ์ | พฤติกรรม |
|---|---|
| เริ่ม Trip ไม่มี SD | เริ่มและบันทึกลง Flash ได้ตามปกติ |
| Trip online และ server รับได้ | commit ข้อมูลใหม่ลง Flash แล้วส่งทันทีตาม sample/event; รับ ACK แล้วทำเครื่องหมายคืนพื้นที่ได้ |
| ไม่มี Wi-Fi หรือ server เข้าไม่ได้ | เก็บ pending records ใน Flash และ retry ตาม backoff |
| กลับมา online | ส่งข้อมูลใหม่ต่อ พร้อมทยอยส่ง backlog ที่ยังไม่ ACK จาก Flash/SD |
| Flash ต้องคืนพื้นที่และมี SD เขียนได้ | ย้ายข้อมูลที่ยังเก็บอยู่ของทริปเก่าที่จบแล้วไป SD; ตรวจสำเนาก่อนลบต้นฉบับใน Flash |
| Flash เต็มและ SD ไม่มี/ใช้ไม่ได้ | ทิ้งทริปที่จบแล้วที่เก่าที่สุดทั้งทริปตามลำดับ จนพอรับข้อมูลใหม่ พร้อมบันทึกเหตุการณ์ข้อมูลถูกทิ้ง |
| เสียบ Notebook/PC | แสดง USB MSC ที่มี CSV จากข้อมูลคงเหลือใน Flash และ SD ตาม snapshot; ไม่ต้องติดตั้งแอปเพื่อคัดลอกไฟล์ |

คำว่า online ต้องหมายถึงเชื่อมเครือข่ายและเข้าถึงบริการ server ที่ยืนยันรับข้อมูลได้ ไม่ใช่เพียง Wi-Fi connected
คำว่า "ส่งตลอดเวลา" ใช้ความหมายว่าเมื่อมี sample/event ใหม่ให้ส่งโดยไม่รอรอบ upload 5 นาทีอีกครั้ง; sample rate เป็น config แยกต่างหาก

ข้อแตกต่างที่ต้องสื่อสารในแอปและ CSV:

- 31 วันคือ offline backlog capacity ตาม profile ไม่ใช่การเก็บสำเนาใน Flash ย้อนหลัง 31 วันเสมอ
- ข้อมูลที่ server ACK และถูก reclaim ไปแล้วอาจไม่อยู่ใน CSV ที่อ่านจากเครื่อง เว้นแต่ยังมีสำเนาใน SD
- ถ้าต้องการประวัติที่ส่งสำเร็จครบทุก Trip ให้ดึงจาก server; การทำ SD mirror ทุก record เป็น option เพิ่มเติม ไม่ใช่สิ่งที่รับรองโดย overflow policy นี้

### 9.2 Partition plan สำหรับ Flash 16 MiB

[HW] ESP32-S3-WROOM-1U-N16R8 มี Flash 16 MB; แผนนี้ใช้ address space 0x000000 ถึงก่อน 0x1000000 หรือ 16 MiB
[PROPOSED] จัด OTA สองช่อง ช่องละ 3 MiB และพื้นที่ log 9.75 MiB; ค่าเหล่านี้เป็นแผนสำหรับ Claude นำไป build/ทดสอบ ไม่ใช่ผลวัด firmware binary จริง

| Partition | Offset | Size | หน้าที่ |
|---|---|---|---|
| bootloader + table | 0x000000 | 0x009000 | พื้นที่ระบบ; partition table เริ่ม 0x8000 ใน config นี้ |
| nvs | 0x009000 | 0x004000 / 16 KiB | Wi-Fi/config/calibration ที่มีขนาดจำกัด |
| otadata | 0x00D000 | 0x002000 / 8 KiB | เลือก OTA slot |
| phy_init | 0x00F000 | 0x001000 / 4 KiB | reserve สำหรับ PHY init ตาม build config |
| nvs_keys | 0x010000 | 0x001000 / 4 KiB | reserve สำหรับ NVS encryption ถ้าเปิดใช้งาน |
| alignment reserve | 0x011000 | 0x00F000 / 60 KiB | เว้นระยะก่อน app; ไม่นับเป็น log capacity |
| ota_0 | 0x020000 | 0x300000 / 3 MiB | Firmware A |
| ota_1 | 0x320000 | 0x300000 / 3 MiB | Firmware B / OTA rollback |
| log_meta | 0x620000 | 0x010000 / 64 KiB | journal/checkpoint/ACK/retention metadata |
| trip_log | 0x630000 | 0x9C0000 / 9.75 MiB | persistent binary log หลัก |
| coredump | 0xFF0000 | 0x010000 / 64 KiB | diagnostic reserve; เปิดใช้เมื่อขนาด dump จริงพอ |

CSV ที่เสนอสำหรับ `partitions.csv`:

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x009000, 0x004000,
otadata,  data, ota,     0x00D000, 0x002000,
phy_init, data, phy,     0x00F000, 0x001000,
nvs_keys, data, nvs_keys,0x010000, 0x001000,
ota_0,    app,  ota_0,   0x020000, 0x300000,
ota_1,    app,  ota_1,   0x320000, 0x300000,
log_meta, 0x40, 0x00,    0x620000, 0x010000,
trip_log, 0x40, 0x01,    0x630000, 0x9C0000,
coredump, data, coredump,0xFF0000, 0x010000,
```

- `0x40` เป็น custom partition type ของโปรเจกต์; subtype 0x00/0x01 แยก metadata/log ไม่ใช่ filesystem FAT ที่ยกให้ PC เขียนโดยตรง
- `ota_0` และ `ota_1` ต้องใส่ signed/encrypted image ที่ผลิตจริงได้ทั้งหมด หากไม่พอให้แก้แผนและคำนวณความจุใหม่ก่อนถือว่าผ่าน
- ไม่ใส่ factory app เพิ่มในแผนนี้; ต้องทดสอบ initial boot, OTA rollback และ USB recovery ตาม build ที่เลือก
- bootloader/partition-table offsets ต้องตรง build config; ตรวจ overlap, alignment และขนาดด้วย ESP-IDF partition tool จริง
- การ reserve `nvs_keys` ไม่ได้เปิด encryption อัตโนมัติ; อย่าเขียน eFuse โดยอัตโนมัติ
- ไม่ใช้ NVS เป็นที่เก็บ telemetry รายเดือน; `log_meta` เป็น journal หมุนเวียนและ index สร้างใหม่ได้จาก `trip_log`
- แผนนี้ไม่กัน Flash อีกชุดเพื่อทำสำเนา CSV ทั้งหมด ใช้ virtual read-only USB volume ตามหัวข้อ 10.3
- เมื่อเปลี่ยน partition layout ของเครื่องที่มีข้อมูลอยู่ ต้องมี export/migration/recovery plan; อย่าลบทั้ง Flash เงียบๆ

ข้อกำหนด alignment, custom type, OTA/NVS และวิธีตรวจตารางอ้างอิง [Espressif Partition Tables](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/partition-tables.html)

### 9.3 งบพื้นที่สำหรับ 31 วัน

[PROPOSED] compact binary frame ไม่เกิน 128 bytes รวม record header/CRC/commit marker; ใช้ ID อ้าง Trip header แทนการเขียน string ซ้ำทุก record
เพื่อคำนวณแบบเผื่อ overhead ให้ใช้ flash sector 4,096 bytes, sector header 64 bytes, 31 frames ต่อ sector และเหลือ padding 64 bytes
ขนาด erase จริงต้องอ่าน/ตรวจจากชิปและ ESP-IDF ที่ใช้; ถ้า geometry ต่างต้องคำนวณใหม่

| รอบ sample | จำนวน sample ใน 31 วัน | ข้อมูล 128 B/sample | พื้นที่รวม sector framing |
|---|---|---|---|
| 300 s / 5 นาที | 8,928 | 1,142,784 bytes | 1.125 MiB |
| 60 s / 1 นาที | 44,640 | 5,713,920 bytes | 5.625 MiB |
| 10 s | 267,840 | 34,283,520 bytes | 33.750 MiB - เกิน Flash ที่จัดไว้ |

ตัวเลขในตารางเป็น periodic samples เท่านั้น ยังไม่รวม event records, Trip metadata, การแยก sector ต่อ Trip และ reserve
ค่าพื้นที่ของ CSV จะมากกว่า binary จึงห้ามเอา 128 bytes ไปใช้คำนวณขนาด CSV

เกณฑ์ออกแบบเริ่มต้นที่เสนอสำหรับพิสูจน์ 1 เดือน:

- Flash log 9.75 MiB = 2,496 sectors; ใช้ไม่เกิน 75% หรือ 1,872 sectors / 7.3125 MiB เป็น budget ข้อมูล ปล่อย 25% สำหรับ reclaim/rotation/headroom
- จำลอง offline 31 วัน: sample ทุก 300 s, event รวมไม่เกิน 256 frames/วัน, Trip ไม่เกิน 32 Trip/วัน และ metadata header+footer 2 frames/Trip
- จำนวนรวม = 8,928 + 7,936 + 1,984 = 18,848 frames; จำนวน Trip สูงสุด 992
- หากแต่ละ Trip ใช้ sector ของตนเอง upper bound จากการปัดเต็ม sector คือ ceil(18,848/31) + 992 - 1 = 1,599 sectors หรือประมาณ 6.2461 MiB
- จึงอยู่ใน budget 7.3125 MiB ตามสมมติฐานนี้ โดยไม่พึ่ง Wi-Fi, SD หรือ compression
- 256 events/วัน และ 32 Trip/วันเป็น qualification profile ที่เสนอ ไม่ใช่คำสั่งให้ทิ้ง event เมื่อถึงจำนวนนี้; หากมากกว่านี้ให้ใช้ retention policy และรายงาน remaining capacity
- ถ้าเพิ่ม sample rate, เก็บ raw GNSS/NMEA, accelerometer waveform, ข้อความยาว หรือเพิ่ม record size ต้องคำนวณใหม่; ห้ามรับรอง 1 เดือนกับอัตราข้อมูลไม่จำกัด

ตัวเลขข้างต้นเป็นการคำนวณ ไม่ใช่ผลรัน firmware 31 วัน; ให้ Claude ส่งเครื่องมือจำลอง/ผลทดสอบและ bytes-per-record จริงประกอบ

### 9.4 Record schema และความทนต่อไฟดับ

[PROPOSED] schema มี version และใช้ logical fields ชุดเดียวกันกับ server/CSV แต่รูปแบบที่เก็บจริงเป็น binary:

- schema_version, record_type, trip reference, record_sequence
- timestamp_utc, time_quality, monotonic_tick
- temperature_raw, temperature_c, temperature_valid, thermocouple_fault
- door_raw, door_state, acceleration/event summary และ peak_valid
- latitude/longitude แบบ scaled integer, gnss_fix_valid, fix_age_s, satellites, hdop
- battery_soc_pct, battery_voltage_mV, battery_current_mA
- source_class, dock_identity, charger_state/fault, alarm/reset/wake/sensor-health bits
- length, CRC และ commit state; fixed framing ต้องรวม overhead ทั้งหมดใน 128 bytes ที่ใช้คำนวณ

Device ID, Trip ID เต็ม, config/calibration version และข้อมูลเริ่ม Trip อยู่ใน Trip header/metadata; ในแต่ละ frame ใช้ reference ที่ตรวจสอบได้
กำหนดขนาด field, signedness, byte order, scaled units และ reserved values เป็นเอกสาร; อย่า serialize raw C struct ที่ขึ้นกับ padding/compiler
หาก event ใช้หลาย frame ให้คิดพื้นที่ทุก frame และกำหนด maximum payload ชัดเจน

- บันทึก append-only ผ่าน storage manager กลาง; เขียน payload/CRC และ commit marker ตาม protocol ที่กู้คืนได้
- หลัง reboot สแกนเฉพาะ record ที่สมบูรณ์/CRC ผ่าน; record ท้ายที่เขียนขาดห้ามถูกส่งเป็น sample ปกติ
- marker/checkpoint ที่เขียนหลายขั้นต้องออกแบบให้ถูกกับ flash programming และ encryption granularity; ห้ามสมมติว่า write ทุกขนาด atomic
- ใช้ circulating sectors และกระจาย erase; metadata journal ต้องหมุนเวียนด้วย ไม่เขียนทับ sector เดียวทุก ACK
- Flash API ไม่ได้ทำ wear levelling ให้ custom log โดยอัตโนมัติ; ต้องมี allocator/reclaim policy และทดสอบ power cut ระหว่างเขียน/ลบ
- ไม่ erase sector ที่ยังมี unacknowledged data ซึ่งยังไม่ได้ย้ายไป SD หรือยังไม่ได้รับการอนุมัติทิ้งทั้ง Trip ตาม retention policy
- sector ที่ ACK ครบคืนพื้นที่ได้; ห้ามลบ unacknowledged record ที่อยู่ร่วม sector เพื่อความสะดวก
- Trip header/metadata ที่ active Trip หรือ records ที่เหลือยังอ้างถึง ต้องคงไว้หรือ checkpoint สำเนาที่กู้คืนได้ก่อน reclaim แม้ตัว header จะถูก ACK แล้ว
- PSRAM/RAM ใช้ buffer/index ได้แต่ไม่ใช่สำเนาถาวร; ระบุ maximum uncommitted data-loss window และ flush critical events
- `invalid` ใช้ validity flag ไม่แทนทุกกรณีด้วย 0; export เป็นช่องว่างและมี status กำกับ
- แยก diagnostic debug log จาก Trip records และให้มี bounded capacity ของตนเอง

ใช้ API ที่จำกัดขอบเขต partition และเคารพ erase/write alignment ตาม [Espressif Partitions API](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/storage/partition.html)

### 9.5 Server synchronization และ ACK

[REQ] เมื่อ Trip online ให้ส่งข้อมูลใหม่ต่อเนื่อง เมื่อกลับจาก offline ให้ทยอยส่งข้อมูลค้างแล้วลบสำเนาที่ไม่ต้องเก็บต่อหลัง server ยืนยัน
[PROPOSED] ลำดับสำหรับแต่ละ record/batch:

1. append และ commit ลง Flash ก่อนนำเข้าคิวส่ง
2. ส่ง batch ที่มี Device ID, Trip ID, sequence/range, schema version และ checksum/request ID
3. server ตอบ application ACK ว่าเก็บข้อมูลถาวรแล้ว พร้อม record IDs หรือ range ที่ยืนยันจริง
4. commit ACK checkpoint ใน metadata ก่อนเปลี่ยน record เป็น reclaimable
5. reclaim แบบเป็นช่วงตาม sector โดยไม่กระทบ pending records หรือข้อมูลที่ USB snapshot กำลังอ้างอิง

- Wi-Fi connected, TCP success, HTTP 200 ที่ไม่ตรง ACK contract หรือ MQTT PUBACK เพียงอย่างเดียว ไม่เท่ากับยืนยันว่า application server บันทึกข้อมูลแล้ว
- ACK บางส่วนทำให้ลบได้เฉพาะส่วนที่ยืนยัน; ถ้าใช้ high-water mark ต้องหมายถึงช่วงต่อเนื่อง ไม่มีช่องโหว่
- ACK ต้องผูกกับ Device/Trip/sequence และตรวจ bounds; ไม่ให้ response ผิด Trip ทำให้ลบ log ของอีก Trip
- ใช้ idempotency key เช่น device_id + trip_id + record_sequence; ส่งซ้ำได้หลัง timeout/reset โดย server ไม่สร้างข้อมูลซ้ำ
- หลัง crash ระหว่าง ACK กับ checkpoint ยอมส่งซ้ำได้ แต่ห้ามเกิดการลบข้อมูลที่ server ยังไม่ได้ยืนยัน
- backlog อ่านจาก Flash และ SD ที่กำลังเข้าถึงได้ เรียงทริป/sequence เก่าก่อน; จัด quota แยกให้ live records ไม่ต้องรอ backlog ทั้งเดือนส่งจนหมด
- จำกัด batch เช่น 32 records เป็นค่าเริ่มต้นที่เสนอ ปรับตามขนาด payload/ACK/server limits; ทำต่อหลาย batch ขณะ online ไม่จำกัดให้ได้เพียง batch เดียวทุก 5 นาที
- upload throughput ระยะยาวต้องมากกว่าอัตรา record ใหม่จึงระบาย backlog ได้; ใส่ retry/backoff, timeout และ rate limit เพื่อไม่แย่ง sensor/USB tasks
- หาก SD ที่มี pending records ถูกถอด ให้แสดงว่ามี pending data บนการ์ดที่ยังไม่เข้าถึง ไม่อ้างว่า sync ทั้งหมดเสร็จ
- แยก `server_acked`, `moved_to_sd`, `exported_to_pc` เป็นคนละสถานะ; การอ่านไฟล์ CSV ผ่าน USB ไม่ใช่ server ACK

### 9.6 Flash retention และ optional SD

[REQ] เมื่อพื้นที่ไม่พอให้จัดการข้อมูลเก่าก่อน โดยทิ้งทีละ Trip; หากมี SD ให้ย้ายไปเก็บที่ SD ก่อน
[PROPOSED] เลือกทริปที่จบแล้วตาม `trip_start_sequence` เก่าที่สุด ใช้เวลา UTC ประกอบได้แต่ไม่ให้ RTC ที่ผิดเวลาทำลำดับพัง

ลำดับคืนพื้นที่:

1. reclaim record/sector ที่ server ACK ครบและไม่มี reader pin ก่อน
2. หากยังต้องการพื้นที่ เลือก completed Trip ที่เก่าที่สุดซึ่งไม่ถูก USB snapshot pin
3. ถ้า SD writable และมีพื้นที่ ให้คัดลอก records/metadata ของ Trip ที่ยังเก็บอยู่ไป temporary archive, sync, ตรวจ CRC/จำนวน records/sequence ranges แล้ว commit manifest
4. เมื่อสำเนา SD กู้คืนได้จริงจึงบันทึกสถานะย้ายสำเร็จ แล้วลบต้นฉบับของ Trip ใน Flash; ไฟดับก่อนขั้นนี้ต้องยังเหลือสำเนาที่ใช้ได้อย่างน้อยหนึ่งชุด
5. ถ้าไม่มี SD หรือ SD unavailable/full/write-failed จนไม่สามารถย้ายได้ ให้ทิ้งข้อมูลคงเหลือของ completed Trip นั้นทั้งทริปตามที่เจ้าของกำหนด แล้วทำซ้ำเท่าที่จำเป็น
6. ก่อนทิ้งข้อมูลที่ยังไม่ ACK ให้ commit loss/tombstone record ในพื้นที่ metadata ที่กันไว้: Trip ID, sequence range, count, reason และเวลาที่ทราบ; ส่ง loss notice ไป server ภายหลัง

- อนุญาตให้ทิ้งทริปที่ยังส่งไม่ครบเมื่อ Flash เต็มตาม requirement นี้ แต่ต้องแสดงว่าเกิด data loss; ห้ามรายงานเป็น upload success
- เหตุผลที่ใช้ได้ เช่น FLASH_RETENTION_NO_SD, SD_FULL, SD_WRITE_FAILED; เก็บ cumulative loss counters และ bounded recent loss journal แยกจาก Trip ที่กำลังลบ
- การ reclaim หลัง server ACK เป็นการคืนสำเนาที่ส่งสำเร็จแล้ว ส่วนการทิ้งข้อมูลที่ยังไม่ ACK เพราะเต็มต้องเลือกทั้ง Trip ห้ามตัดเฉพาะ record เก่าสุดของหลาย Trip
- SD ไม่มีตั้งแต่เริ่มใช้งานเป็นสถานะปกติ ไม่บล็อก boot/START_TRIP และไม่ส่ง alarm ซ้ำๆ ว่าไม่มีการ์ด
- SD ที่มีอยู่แต่ใช้งานพลาดต้องรายงาน; ไม่ auto-format การ์ดหรือข้อมูลใน Flash เมื่อ mount/recovery fail
- SD ใช้ SPI2/FAT ตาม hardware; เปิด rail เมื่อใช้งาน และ flush/unmount ก่อนปิด rail ซึ่งแชร์กับ GNSS
- SD archives ที่ย้ายไว้ยังเป็นแหล่งข้อมูลให้ uploader/USB CSV; หลัง server ACK ไม่จำเป็นต้องลบ SD archive ทันทีใน baseline นี้
- [VERIFY] ยังไม่ได้กำหนดให้ลบ archive บน SD อัตโนมัติเมื่อ SD เต็ม; baseline ให้รายงาน SD_FULL แล้วใช้ Flash eviction policy ข้างต้น โดยไม่ลบไฟล์ SD อื่นเอง
- [VERIFY] SD overflow archive อาจเป็นสำเนาไม่ครบทั้งประวัติของ Trip ถ้า record บางส่วนถูก server ACK และ reclaim ไปก่อนแล้ว; manifest ต้องมี complete_local_copy และ available ranges ตามจริง

กรณีขอบเขตที่ต้องระบุชัด:

- [PROPOSED] ไม่ลบ Trip ที่กำลังทำงาน และไม่ลบ data ที่ reader/USB snapshot pin อยู่
- ถ้าไม่มี eligible completed Trip เหลือ หรือ Trip เดียวใหญ่เกินความจุ และ SD ช่วยไม่ได้ ให้แจ้ง STORAGE_FULL, เก็บ loss/gap counters และไม่แอบเปลี่ยนไปลบครึ่ง Trip
- กรณีนี้รับประกันการบันทึกข้อมูลใหม่ต่อไม่ได้จนมี ACK/พื้นที่/SD หรือจบ Trip; ห้ามอ้างว่าเก็บได้ไม่จำกัด
- automatic Trip rollover/การแบ่ง Trip ยาวเป็นช่วงต่อเนื่องเป็นนโยบายที่ยังต้องยืนยัน ไม่ให้ Claude เปลี่ยนขอบเขต Trip เอง

### 9.7 Status ที่แอป/จอ/CSV ต้องอ่านได้

- flash_total/free/reclaimable/pinned bytes และ estimated offline retention ตามอัตราข้อมูลจริง
- pending records/bytes แยก Flash/SD, oldest pending Trip และ last successful server ACK
- SD status: NOT_PRESENT / READY / FULL / ERROR / REMOVED_WITH_PENDING_DATA
- trip local completeness, storage location และ sequence ranges ที่ยังมี
- dropped Trip/record counts, last retention reason และ known data gaps
- USB snapshot time, record boundary, pinned bytes และ snapshot generation

## 10. BLE, NFC, USB และ Wi-Fi

### 10.1 BLE

[REQ] รองรับ iOS app
[PROPOSED] ใช้ BLE GATT บน NimBLE/ESP-IDF ตาม version ที่เลือก

Service groups ที่เสนอ:

- device information
- live status/telemetry
- config read/write
- command/control
- event notification
- log transfer

ต้องออกแบบ:

- UUID table และ payload schema อย่างเป็นเอกสาร
- BLE MTU-independent chunking, sequence, acknowledgement, timeout และ resume
- authorized session สำหรับเปลี่ยน config/START/STOP/delete/update
- bounds checking และ version negotiation
- advertise Device ID/service identifier ที่เชื่อมโยงกับ NFC
- session timeout และ power saving
- reconnect โดยไม่ทำ Trip/log หาย
- ไม่มี claim ว่า NFC ทำ iOS BLE pairing อัตโนมัติทุกกรณี; app เป็นผู้ดำเนิน workflow

Deep sleep ตัด BLE connection และ advertising:

- โหมดประหยัดแบตตื่นเป็นช่วง หรือใช้ NFC GPO ปลุกเมื่อ SW3 ยัง ON
- โหมด BLE interactive คงการเชื่อมต่อด้วย power strategy ที่รองรับ และยอมรับกระแสเฉลี่ยสูงขึ้น
- remote BLE command ไม่สามารถปลุกจาก deep sleep ได้ถ้าวิทยุปิดอยู่
พฤติกรรม sleep อ้างอิง [ESP-IDF Sleep Modes](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/sleep_modes.html)

### 10.2 NFC

[HW] ST25DV04KC เป็น dynamic NFC tag ไม่ใช่ NFC reader

- ทำ NDEF device identity/app URL หรือ deeplink ตาม app ที่ใช้จริง
- เขียนเฉพาะข้อมูลเปลี่ยนเพื่อลด EEPROM wear
- 4 Kbit EEPROM ประมาณ 512 bytes ต้องจัด capacity สำหรับ NDEF/overhead; ไม่ใส่ทั้ง Trip log
- mailbox เป็น optional เมื่อจำเป็นและ driver ยืนยันถูกต้อง
- รับ GPO event และอ่าน source/status
- timestamped last status หากนำไปใส่ NDEF ต้องแสดงว่าอาจเป็น cached data
- passive RF อาจอ่าน NDEF ได้เมื่อเครื่องปิด แต่ไม่เท่ากับ ESP32 ทำงาน/เปิด BLE/เริ่ม Trip ได้
- V_EH ไม่ต่อ; ไม่สมมติว่า RF energy เลี้ยงทั้งเครื่อง
- ป้องกันคำสั่งเขียนที่เปลี่ยนการชาร์จหรือลบข้อมูลจาก NFC โดยไม่มี authorization

### 10.3 USB Flash Drive และ CSV

[REQ ล่าสุด] เสียบ Device กับ Notebook/PC ผ่าน USB data cable แล้วต้องเห็นเป็น removable USB drive และคัดลอกไฟล์ CSV ได้ แม้ไม่ใส่ SD card
[HW] native USB ใช้ GPIO19 D-, GPIO20 D+; ESP32-S3 เป็น USB Full-Speed
[PROPOSED] ใช้ TinyUSB Mass Storage Class (MSC) เป็น interface หลักสำหรับไฟล์ และเพิ่ม CDC/vendor interface เป็น optional สำหรับ config/diagnostics

พฤติกรรมที่ผู้ใช้เห็น:

- volume label เช่น FOAM_12AB34 และ USB serial คงที่ ไม่ซ้ำกันแต่ละ Device
- มี CSV แยกตาม Trip ที่ยังมีข้อมูลใน Flash หรือ SD เช่น TRIP_000123.csv; รวมสำเนาซ้ำข้าม storage ด้วย Trip ID/sequence
- มี TRIPS.csv เป็นสารบัญ: Trip ID/start/end, records available, pending/ACK status, complete_local_copy และ storage location
- มี DEVICE.csv หรือ README.txt บอก Device ID, firmware version, snapshot time, หน่วยและ timezone
- Trip ที่ยังทำงาน export ได้ถึง snapshot boundary พร้อมสถานะ ACTIVE/PARTIAL; ไม่ใส่ record ที่ยังเขียนไม่สมบูรณ์
- ถ้าไม่มี log ให้ยัง mount drive ได้และมีข้อมูลสถานะ/README ไม่ขึ้นเป็นสื่อเสียเพราะไม่มี SD
- CSV เป็นข้อมูลที่ยังเก็บอยู่จริง ไม่สามารถสร้าง record ที่ reclaim ไปแล้วจาก Flash และไม่มีสำเนา SD กลับมาได้

Snapshot และ filesystem ownership:

1. เมื่อ attach ให้สร้าง export snapshot จาก committed records/Trip metadata/ACK state ณ เวลานั้น และ pin แหล่งข้อมูลที่ต้องอ่าน
2. เสนอเป็น virtual FAT volume แบบอ่านอย่างเดียว; FAT directory, file sizes และเนื้อหาแต่ละ sector ต้องคงที่ตลอด snapshot
3. แปลง binary records เป็น CSV ตามคำขออ่าน block ของ host ใช้ buffer/index แบบมีขอบเขต; ไม่สร้างสำเนา CSV ทั้งเดือนลง Flash อีกชุด
4. logger เขียน record ใหม่ต่อในพื้นที่ที่ไม่ชน snapshot; uploader ส่งต่อได้ แต่ต้องเลื่อน physical erase ของข้อมูลที่ snapshot pin ไว้
5. หลัง host eject หรือ USB detach จึงปล่อย pin และ reclaim; รอบ mount ถัดไปสร้าง snapshot ใหม่เพื่อเห็นข้อมูลล่าสุด

- Firmware ต้องทำ virtual FAT/block provider และ deterministic CSV mapping เองหรือเลือก library ที่ตรวจแล้ว; เปิดตัวอย่าง MSC บน raw `trip_log` อย่างเดียวไม่ทำให้ PC อ่าน CSV ได้
- รองรับ READ10 ที่อ่านไม่เรียงลำดับ อ่านซ้ำ อ่านบางส่วน และ block ที่คร่อมขอบ CSV row โดยคืนข้อมูลชุดเดิมทุกครั้ง
- เตรียม file sizes/sector mapping ให้แน่นอนก่อนแจ้ง media ready; ใช้ bounded/sparse index เมื่อต้องอ่าน archives จำนวนมากจาก SD
- ส่ง write-protected status และตอบ WRITE10/format/write attempts ให้ถูกต้อง; host ไม่ได้รับสิทธิ์ลบ log หรือ config ผ่านการแก้ directory
- ไม่แชร์ FAT/SD volume ที่ mount เพื่อเขียนพร้อมกันระหว่าง host กับ Firmware; virtual export ต้องแยกจาก underlying storage ที่ app จัดการ
- หาก SD ที่ snapshot ใช้อ่านถูกถอด ให้คืน media/read error ตามจริง ห้ามคืนศูนย์เป็นข้อมูล CSV สำเร็จ; แจ้งสถานะและสร้าง snapshot ใหม่เมื่อ remount
- กันพื้นที่สำหรับ log ใหม่ระหว่างเสียบ USB ตาม budget; ถ้าข้อมูลทั้งหมดที่คืนพื้นที่ได้ถูก pin และพื้นที่หมด ให้แจ้ง STORAGE_FULL/USB_SNAPSHOT_PINNED ตาม policy ห้าม erase ขณะ PC กำลังอ่าน
- ห้ามบังคับ STOP_TRIP เพียงเพราะเสียบ USB; ถ้าต้องมี maintenance mode ต้องสั่งชัดเจน
- การเปิด/คัดลอก CSV ไม่มี application ACK ว่า server รับข้อมูลแล้ว จึงไม่ใช้เป็นเงื่อนไขลบ pending data อัตโนมัติ
- หากต้องการ refresh ไฟล์ระหว่างยังเสียบ ต้องทำ explicit eject/remount/media-change protocol ที่ทดสอบกับ OS; ห้ามเปลี่ยน FAT/file content ใต้ cache ของ host

CSV format ที่เสนอ:

- UTF-8 พร้อม BOM เพื่อความสะดวกใน Excel บน Windows, comma delimiter, CRLF และ header row ที่คงที่ตาม schema version
- เวลา UTC แบบ ISO 8601 ลงท้าย Z; ถ้าจะมีเวลาไทยให้เพิ่ม column แยกพร้อม offset ไม่เปลี่ยนความหมาย UTC column เดิม
- numeric ใช้ decimal point แบบคงที่; invalid value เป็นช่องว่างพร้อม validity/fault column ไม่ใช้ 0 ปลอมเป็นค่าที่วัดได้
- quote/escape comma, quote และ newline ถูกต้อง; field ข้อความจากผู้ใช้ต้อง export เป็นข้อความอย่างปลอดภัย ไม่ให้ถูกตีความเป็น formula ใน spreadsheet
- columns อย่างน้อย: schema_version, device_id, trip_id, record_sequence, timestamp_utc, time_quality, record_type, temperature_c, temperature_valid, door_state, latitude, longitude, gnss_fix_valid, battery_soc_pct, battery_voltage_mV, battery_current_mA, alarm_bits, sensor_health_bits, server_acked_at_snapshot
- event records ใช้ record_type และ event_code พร้อม field ที่เกี่ยวข้อง; การแปลง CSV ต้องรักษาหน่วยและค่าที่บันทึกจริง
- completion/gap information อยู่ใน TRIPS.csv/README; ห้ามเรียกไฟล์ที่มีเพียง records คงเหลือว่าเป็นประวัติ Trip ครบถ้วน

USB integration:

- USB drive ใช้ได้เมื่อ SW3 ON และ Firmware ทำงาน; SW3 OFF ตัด 3V3_MAIN จึงไม่สามารถให้ MCU enumerate เป็นไดรฟ์ได้ตาม hardware ปัจจุบัน
- รองรับ Device 6 ตัวผ่าน Dock เป็น 6 ไดรฟ์อิสระ พร้อม serial/volume identity แยกกัน; ทุก Device มี Firmware ของตนเอง ไม่เพิ่ม MCU ให้ Dock
- USB enumeration/data มีข้อจำกัดเรื่องแหล่งจ่าย Dock ตามหัวข้อ 6; MSC ไม่ได้แก้ hardware net ของ Dock
- CDC control ถ้ามีให้แยก debug log กับ binary protocol และมี parser/version/CRC/request ID ตามเดิม; USB file copy ต้องใช้ได้โดยไม่พึ่ง CDC app
- VBUS/detach/suspend handling ให้ตรงวงจรจริง; ไม่ใช้ D+/D- เป็น UART และไม่สัญญาความเร็ว 480 Mbps
- USB-OTG/TinyUSB และ USB-Serial-JTAG แชร์ internal PHY ใน hardware นี้; แยก production MSC mode กับ flashing/debug/recovery mode
- ไม่ program irreversible eFuse เพื่อแก้ USB workflow โดยอัตโนมัติ; GPIO0 recovery access ยังต้องตรวจตาม schematic

ความสามารถ MSC และข้อกำหนด USB stack อ้างอิง [Espressif USB Device Stack](https://docs.espressif.com/projects/esp-usb/en/latest/esp32s3/usb_device.html); virtual CSV snapshot ข้างต้นเป็น design ของโปรเจกต์ที่ต้องพัฒนาและทดสอบเพิ่ม

### 10.4 Wi-Fi และ server

[REQ ล่าสุด] Wi-Fi เป็นช่องทางหลักในการส่งข้อมูล Trip ไป server ไม่ใช่ optional feature
[HW] ESP32-S3 รองรับ 2.4 GHz Wi-Fi
[VERIFY] SSID/provisioning, endpoint, credentials, HTTPS/MQTT และ application ACK schema ยังไม่ถูกระบุ

- หลัง START_TRIP พยายามเชื่อมเครือข่ายที่ตั้งค่าไว้; เมื่อ server พร้อมให้ส่ง sample/event ใหม่ทันทีและระบาย backlog เป็น batch ต่อเนื่อง
- ขณะ online คง network session ตาม power profile ที่รองรับ; ห้าม deep sleep ตัด Wi-Fi ทุก 5 นาทีแล้วอ้างว่ายังคง online ต่อเนื่อง
- เมื่อ offline หรือ server error ให้บันทึกลง Flash และ reconnect/retry แบบมี deadline/backoff; ไม่วนเชื่อมไม่หยุดจนแบตหมด
- แยก sensor sample interval, Wi-Fi reconnect interval, live upload trigger และ backlog batch scheduling ออกจากกัน
- เมื่อ Trip หยุดแต่ยังมี pending data ให้ upload ต่อเมื่อเชื่อมต่อได้และ power policy อนุญาต เช่นตอนชาร์จ/Dock; ไม่บังคับให้เริ่ม Trip ใหม่เพื่อส่งของเก่า
- upload task ไม่บล็อก sensor, Flash logger หรือ USB MSC; capacity counters/ACK status ต้องมาจาก storage manager ชุดเดียวกัน
- endpoint/credentials ผ่าน configuration; ใช้ TLS และตรวจ server certificate เมื่อ protocol รองรับตามที่เลือก ไม่ hardcode ปลายทางสมมติ
- MQTT QoS/HTTP response semantics ต้องตรง application ACK contract ในหัวข้อ 9.5
- ESP-NOW/custom gateway เป็น future option แยกจาก Wi-Fi-to-server requirement นี้; ไม่ใช้แทนโดยเงียบๆ
- ไม่มี LoRa/4G hardware ใน revision นี้

## 11. Alarm และค่าเริ่มต้น

[PROPOSED] alarm engine ควรแยก condition, active, latched, acknowledged, cleared

Events ที่ควรรองรับ:
temperature high/low, probeopen, door opened, door open too long, shock/motion ตาม config, low/critical battery, RTC invalid, Flash write/recovery/full, retention data loss, SD full/write error หรือถอดการ์ดที่มี pending data, GNSS no-fix/stale ตาม policy, sensor bus error, charge fault, reboot/watchdog/brownout

การหายไปของ GNSS ในอาคารไม่จำเป็นต้องเป็น critical alarm ทุกกรณี ให้กำหนดระดับได้
ค่า stale/invalid temperature ไม่ควรถูกแสดงเป็น normal temperature
เก็บ alarm history แม้ผู้ใช้ mute เสียงหรือ ack แล้ว

ค่าที่เสนอสำหรับเริ่มพัฒนา ไม่ใช่ spec ที่ยืนยันทั้งหมด:

| Parameter | ค่าเริ่มต้น/สถานะ |
|---|---|
| offline periodic cycle | 300 s ตามรอบเดิม; online upload ใช้ record-ready event [REQ] |
| temperature sample interval | 300 s [PROPOSED เพื่อให้ตรงรอบตื่นเดิม] |
| door/accel/NFC | event-driven เมื่อ hardware รองรับ [PROPOSED] |
| temperature band | 2-8°C [PROPOSED; config ได้] |
| alarm hysteresis/dwell | ต้องกำหนดตาม precision/response ที่ต้องการ [VERIFY] |
| GNSS interval/acquisition timeout | configurable และต้องอยู่ใน energy budget [VERIFY] |
| display refresh | เฉพาะค่าหรือสถานะเปลี่ยนตาม minimum interval ของ panel [PROPOSED] |
| low/critical battery | configurable; ต้องสัมพันธ์ cell และ system brownout [VERIFY] |
| BLE interactive window | configurable; ไม่เปิด BLE ค้างใน offline transport default [PROPOSED] |
| primary storage | internal Flash; SD optional [REQ] |
| offline retention capacity | 31 วันภายใต้ profile ในหัวข้อ 9.3 [REQ+PROPOSED sizing] |
| live upload | หลัง commit record และ server online [REQ] |
| backlog upload | ต่อเนื่องทีละ batch พร้อม quota สำหรับข้อมูลใหม่ [REQ] |
| USB CSV | MSC read-only snapshot เมื่อเสียบ PC/NB [REQ+PROPOSED implementation] |
| non-Dock ICHG | target<=500 mA -> register 480 mA [REQ+IC constraint] |
| Dock ICHG | target 1200 mA เมื่อ identity และ budget ผ่าน [REQ] |
| >=80% slow charge | threshold แนวคิดเดิม; slow current/hysteresis ยัง VERIFY |

ถ้า sample temperature ทุก 300 s จะตรวจ excursion ช้าถึงประมาณหนึ่ง sample interval และพลาดเหตุการณ์สั้นระหว่าง sample ได้
ห้ามอ้างว่า temperature alarm เป็น real-time ต่อเนื่องด้วย configuration นี้
หากเจ้าของต้องการ alarm เร็วกว่านั้น ให้แยก sample interval กับ transmit interval และคำนวณ runtime ใหม่

## 12. Power management และเป้าหมาย 7 วัน

[REQ] 1500 mAh, เป้าหมาย 7 วัน; รอบ 5 นาทีสำหรับ offline baseline และ online Wi-Fi ตามข้อกำหนดล่าสุด

คำนวณจาก requirement:

- 7 วัน =168 ชั่วโมง
- กระแสเฉลี่ยเชิงอุดมคติสูงสุดที่แบต =1500/168 ≈8.93 mA
- หากกันความจุไว้ 20% และใช้คำนวณเพียง 80% จะเหลือ budget ประมาณ 7.14 mA
- รอบ 5 นาทีมี 288 รอบ/วัน และ 2016 รอบ/7 วัน
- runtime ต้องวัด battery-side จริง รวม conversion loss, sensor, standby, LED, radio, SD และ GNSS
- ความจุจริงที่อุณหภูมิใช้งานต้องยืนยัน ไม่อ้างจากฉลากเพียงอย่างเดียว

[PROPOSED] แบ่ง power profiles:

1. TRIP_OFFLINE_LOW_POWER: timer wake 300 s + event wake, Flash logging, bounded Wi-Fi reconnect, LED off
2. TRIP_WIFI_ONLINE: คง Wi-Fi/server session, ส่ง record ใหม่และระบาย backlog; ใช้ modem/light sleep ที่ compatible ได้ แต่ไม่ deep sleep ตัด session
3. BLE_INTERACTIVE: BLE session พร้อมรับ command, timeout, คิดกระแสเพิ่ม
4. USB_CONNECTED: MSC snapshot/CSV, Flash logging และ optional network upload ทำงานร่วมกัน; ไม่ sleep จน USB หลุด
5. CHARGING: คุม charger/watchdog และเร่ง backlog upload ภายใต้ budget ได้
6. CRITICAL_BATTERY: commit critical state, ลดโหลด, ไม่บูตวน
7. HARD_OFF: SW3 OFF, Firmware ไม่ทำงาน

Wi-Fi online ต่อเนื่องและ USB snapshot ที่อ่าน SD จะเพิ่มการใช้พลังงาน ต้องวัดแยกจากโหมด offline 5 นาที
ไม่อ้างว่า profile online ทำได้ 7 วันเพียงเพราะคำนวณ offline ผ่าน; ความจุ log 31 วันและ battery runtime เป็นคนละเป้าหมาย

รอบทำงานทั่วไป:

1. boot/resume และตั้ง outputs ให้ safe
2. ตรวจ wake/reset reason และ status ที่จำเป็น
3. จัดการ interrupt/events
4. เปิด rail ตามงาน
5. รอ conversion อ่าน sensor/เวลา/แบต
6. GNSS และ radio ทำงานเท่าที่ policy อนุญาตและมี timeout
7. append/commit ลง Flash แล้วส่งข้อมูลใหม่และ sync backlog ถ้า server online; SD ใช้เมื่อ archive/overflow/export ต้องอ่านจริง
8. update จอ/LED เฉพาะจำเป็น
9. flush/release peripherals ที่ไม่ใช้ โดยไม่ตัด session Wi-Fi/USB หรือ snapshot ที่กำลังทำงาน
10. offline profile จึง rearm wakes และ deep sleep; online/USB ใช้ power strategy ที่คง session ได้

Wake:

- GPIO1 NFC, GPIO2 accel, GPIO4 PG, GPIO5 PMIC, GPIO7 door อยู่ในกลุ่ม RTC-capable GPIO ของ ESP32-S3
- periodic wake ใช้ ESP timer เพราะ external RTC interrupt ไม่ต่อ
- เลือก EXT0/EXT1/ULP ตาม capability ของ IDF/SoC จริง
- [PROPOSED] อาจแยก door ที่ต้อง wake ตรงข้ามระดับปัจจุบันไว้ EXT0 และกลุ่ม interrupt active-low ไว้ EXT1 หาก configuration รองรับ
- อย่ารวม active-high กับ active-low ใน EXT1 mask แบบที่ SoC/version ไม่รองรับ
- เมื่อ PG ค้าง LOW เพราะมีไฟอยู่ ห้าม arm ให้ LOW เป็น wake แล้ว sleep วน
- clear latched sensor/NFC interrupt และตรวจสถานะก่อนหลับ
- IRQ pulse ของ charger ไม่ควรเป็นกลไกตรวจ external power เพียงอย่างเดียว
- กำหนด GPIO42/47/48 hold และ default pull อย่างชัดเจน
- PSRAM ไม่ใช่ persistent storage ใน deep sleep/reset

ไม่รับประกัน 7 วันจาก software design อย่างเดียว ให้ส่ง measurement plan และ power budget พร้อมผลที่วัดได้จริงภายหลัง

## 13. Firmware architecture ที่เสนอ

[PROPOSED] ESP-IDF C/C++, FreeRTOS
เลือก stable version ที่ตรวจ compatible กับ ESP32-S3/USB/NimBLE แล้วและ pin version ใน README
ไม่ผสม API จากหลาย IDF version และอย่าระบุว่า compile ผ่านถ้ายังไม่ได้ build

ชั้นงาน:

- board support: pinmap, polarities, board revision, power domain definitions
- drivers: I²C/SPI/UART และ IC drivers
- managers: power, charger/PD, sensors, RTC/time, display, alarms, connectivity
- storage: Flash partition backend, append journal, Trip catalog, ACK/retention, optional SD archive
- export: USB snapshot ownership, virtual FAT/MSC block provider, CSV encoder และ bounded index
- synchronization: Wi-Fi connection, server client, live/backlog scheduler, idempotent ACK handling
- application: Trip/state/config/event routing
- protocols: common commands/payload schema และ transport adapters
- maintenance: diagnostics/update/recovery

ออกแบบ:

- event queue/task notifications แทน blocking delay ยาว
- central rail manager และ bus mutex
- bounds/timeouts/retry limits ทุก external device
- feature flags สำหรับ physical display ที่ยังไม่ผ่านการยืนยัน revision/วงจรขับ, PD 9 V, optional SD/OTA/CDC; Flash primary, Wi-Fi sync และ USB MSC/CSV เป็น core scope
- watchdog บน task ที่เหมาะสม
- NVS config versioning/migration/validation
- sensor health และ degraded mode; อุปกรณ์หนึ่งหายไม่ทำทั้งเครื่อง hang
- dynamic allocation อย่างจำกัดและจัด DMA-capable buffers ตาม ESP-IDF
- log levels และ production quiet mode
- ไม่ทำ network/file I/O/I²C ใน ISR
- ตื่นจาก deep sleep แล้ว restore เฉพาะ state ที่มีแหล่งเก็บจริง

[PROPOSED] commands ขั้นต่ำ:
GET_INFO, GET_STATUS, GET_CONFIG, SET_CONFIG,
SET_TIME, START_TRIP, STOP_TRIP, ACK_ALARM,
LIST_TRIPS, GET_TRIP_SUMMARY, READ_LOG_CHUNK,
GET_STORAGE_STATUS, GET_SYNC_STATUS, SYNC_NOW, GET_USB_SNAPSHOT_STATUS,
SELF_TEST, REBOOT

Optional maintenance:
OTA_BEGIN/CHUNK/COMMIT ตาม transport ที่เลือก,
FACTORY_RESET/DELETE_LOG ที่ต้องยืนยัน intent และไม่เป็น default หลัง error

SET_CONFIG ต้องตรวจ range และไม่ยอมให้ตั้ง charge voltage/current หรือ PD voltage เกิน hardware allowlist
GET_STATUS ต้องแยก unknown/not-supported จากค่า 0
ทุก command มี request ID/status/error code และรองรับ retry โดยไม่ทำ operation ซ้ำผิดพลาด

## 14. Bring-up และ verification ที่ต้องส่งต่อ

ลำดับแนะนำ:

1. ตรวจ power rails/3V3/boot/reset/USB flash โดย bench current limit
2. ตรวจ GPIO/polarity โดยไม่เปิด high-current charge
3. I²C scan เฉพาะ bring-up + chip identification/status
4. RTC/time, fuel gauge, current monitor
5. MAX6675 known-temperature/probe-open
6. door ทั้งสภาพติดแม่เหล็ก/ถอดแม่เหล็ก และ wake
7. LIS2DW12 INT/FIFO/event configuration
8. GNSS UART ตาม TX17 / RX18 และ valid NMEA
9. Flash custom partition/log/recovery ก่อน แล้วทดสอบ SD archive เป็น option รวม power loss ระหว่างย้ายข้อมูล
10. LED chain/color/power off/buzzer
11. e-paper หลังตรวจ revision/FPC/boost circuit; ทดสอบ 4 สี, orientation, BUSY timeout และ rail cycling ร่วม MAX6675
12. BLE/NFC workflow
13. USB MSC/CSV ตรง PC โดยไม่มี SD แล้วทดสอบ optional CDC, Wi-Fi uploader และ Dock 6 ไดรฟ์พร้อมกัน
14. charger policy/PD 9 V หลังตรวจ hardware และ datasheet ที่ยังขาด
15. power measurement และ runtime test

Acceptance cases:

- เปิด/ปิด SW, reset, brownout และ reboot กลาง Trip กู้ข้อมูลได้ตามขอบเขตที่กำหนด
- Probe หลุดไม่ถูก log เป็น normal 0 °C
- Door เปิดค้างไม่ทำ wake storm; polarity ตรง mechanical assembly
- Accel interrupt clear แล้ว sleep ได้; shock record ไม่อ้าง peak ถ้าไม่ได้เก็บ peak จริง
- GNSS no-fix ไม่ hang; stale age ถูกต้อง
- Boot/START_TRIP/logging/USB CSV ทำงานได้เมื่อไม่มี SD card
- จำลอง offline 31 วันตาม record/event/Trip profile หัวข้อ 9.3 อยู่ภายใน budget โดยไม่มี eviction ของข้อมูลที่ควรเก็บครบ
- Flash partition ไม่ overlap; app image จริงพอดี OTA slots; OTA ไม่แก้ Trip log partition
- มี Wi-Fi แต่ server unavailable ไม่ลบ pending data; reconnect แล้วทยอย upload จน backlog ลดได้จริง
- ACK หาย/ซ้ำ/ผิด Trip/บางส่วน/ข้ามช่วง และ reboot ระหว่าง ACK checkpoint ไม่ทำให้ข้อมูลหายหรือ server ซ้ำ
- Flash เต็มเลือกทริปที่จบแล้วเก่าสุด: SD writable ต้องย้ายก่อนลบ; ไม่มี/SD เต็มต้องทิ้งทั้ง Trip พร้อม loss notice
- ไฟดับระหว่าง SD copy/manifest commit/Flash reclaim ต้องกู้ได้อย่างน้อยหนึ่งสำเนาที่ตรวจ CRC ผ่าน
- SD เต็ม/ผิดพลาด/ถอดระหว่าง write หรือ USB export มี fault/recovery ไม่ autoformat
- ทริปเดียวใหญ่เกินพื้นที่หรือ snapshot pin ทำพื้นที่ไม่พอต้องเข้า STORAGE_FULL ตาม policy ไม่ลบครึ่ง Trip เงียบๆ
- Windows Explorer เห็นไดรฟ์และคัดลอก CSV ได้โดยไม่ใช้แอป; Excel/import parser อ่าน UTF-8/หน่วย/invalid values ถูกต้อง
- CSV export ผ่าน random/repeated/unaligned-with-row block reads ได้ byte-for-byte คงที่ระหว่าง snapshot
- USB CSV ไม่มี duplicate records ข้าม Flash/SD และแสดง partial history/data gaps ตามจริง
- Host write/format ถูกปฏิเสธอย่างถูกต้อง; USB read/copy ไม่ถูกตีความเป็น server ACK
- Logger, uploader และ USB export ทำพร้อมกันได้โดยไม่ erase record ที่ snapshot pin; eject/detach ปล่อยพื้นที่ได้
- ปิด shared rail ไม่มี back-power และไม่ตัดธุรกรรมของอีก device
- current sign INA226 ถูกตอน charge/discharge และ calibration ตรง 10 mΩ
- NFC อ่าน ID และเริ่ม BLE workflow ได้; hard-off ไม่ถูกอ้างว่า MCU ตื่นได้
- USB detach/reattach และ legacy/current allowance ผ่านการทดสอบ
- Device 6 ตัวมี serial ไม่ซ้ำและ PC ดาวน์โหลด/ตั้งค่าแยกได้
- PC-only Dock data operation ทดสอบตาม net ที่แก้/ยืนยันแล้ว ไม่ข้ามข้อจำกัด 5V_SYS
- Unknown source/Rp 1.5 A ไม่ถูกระบุว่า Foam Dock โดยไม่มีหลักฐาน
- Charger watchdog/reset ไม่ทำ limits เปลี่ยนเป็นค่าที่ไม่ตั้งใจ
- Firmware ไม่ขอ PD voltage เกิน allowlist
- LED off และ buzzer idle กินไฟตามที่วัดจริง
- Sleep/wake ทุกแหล่ง, event latency และ battery current วัดจริง
- sample/transmit/display/GNSS interval ทำตาม config ไม่ค้าง radio เพราะ retry
- Update ผิดรุ่น/ขาดตอนต้อง recover ตาม strategy ที่เลือก

Unit/integration tests เน้น logic ที่มีความเสี่ยงจริง:
charge-policy arbitration, current quantization/bounds, protocol parser, record CRC/recovery, ACK/idempotency, capacity/whole-Trip eviction, SD copy-then-delete, virtual FAT/CSV random reads, snapshot pin/release, Trip resume และ wake policy
Hardware acceptance ไม่ถูกแทนด้วย mock tests หรือผล compile

## 15. สิ่งที่ยังต้องยืนยันก่อนถือว่า Firmware พร้อมผลิต

| Item | เหตุผล/ผลต่อ Firmware | ทำต่อได้อย่างไรระหว่างรอ |
|---|---|---|
| e-paper G revision/init และ raw FPC/boost circuit | รุ่น/ขนาด/250×122/4 สียืนยันแล้ว แต่ V2 ไม่ยืนยัน; วงจรบางจุดต่างจาก datasheet | ทำ HAL/view model 4 สี; เปิด driver จริงหลังตรวจ revision และวงจร |
| HUSB238A full register map/suffix/default | ต้องตรวจ autonomous PD, reset และ 5 V OFF policy | PD interface, conservative profile, 9 V disabled |
| Dock identity แบบไม่มี PC | Rp22k ไม่ unique | UNKNOWN identity + non-Dock limit |
| Battery datasheet | ICHG/VREG/temp range/capacity จริง | limits config พร้อม validation; high charge ยังไม่รับรอง |
| Battery temperature protection | TS เป็น fixed divider | ระบุ unsupported ไม่ปลอม sensor |
| RTC VBAT_3V source | ไม่เห็น backup source ในหน้า PDF | time validity/re-sync |
| VBUS/self-powered USB detect | PG# ไม่ใช่ comparator 4.54 V โดยนิยาม | power status abstraction + bench verification |
| Dock 5V_SYS / hub 5V_POWER | PC-only power/data ยังไม่เชื่อมตามเป้าหมาย | test ด้วย PD input และทำ hardware issue แยก |
| Dock buck inductor/SY6280 limit/45 W budget | ยังไม่ยืนยัน continuous full-load | static input budget + test |
| Alarm threshold/sample latency | ไม่มี spec ตัวเลขครบ | configurable defaults ติดป้าย PROPOSED |
| Server API/ACK/credentials | Wi-Fi-to-server ยืนยันแล้ว แต่ endpoint/protocol/durable ACK ยังไม่ระบุ | server client interface และ local/mock endpoint สำหรับทดสอบ โดยไม่อ้างว่าเชื่อม production แล้ว |
| Monthly data profile / binary size | ความจุขึ้นกับ sample/events/Trip count; 128 B และ layout เป็นข้อเสนอ | ใช้ qualification profile 31 วันพร้อม capacity simulator และ bytes จริง |
| Firmware image size / partition release | 3 MiB ต่อ OTA slot ต้องตรวจ build จริง | ตรวจ partition tool/image size และ recalculate ก่อนปล่อย |
| Active Trip ใหญ่เกิน Flash / SD เต็ม | ยังไม่กำหนด rollover และนโยบายลบ SD archives | รายงาน STORAGE_FULL; ไม่ลบ active Trip/ไฟล์ SD เอง |
| USB virtual snapshot | ต้องทำ FAT/CSV provider และ concurrency ให้ครบ | MSC read-only พร้อม Flash-only acceptance test |
| BLE UUID / NDEF URL / app flow | ต้องตรง iOS app | เสนอ schema แล้ว document version |
| Door polarity/physical connector | NC ต้องยืนยันกับแม่เหล็กจริง; หน้า PDF ไม่แสดง connector ของ door ชัด | configurable polarity |
| Buzzer active/passive, LED color order | part/timing ต้องตรงของจริง | configurable driver options |
| GPIO0/USB recovery access | ไม่เห็น BOOT button ใน PDF | document flashing/recovery bring-up |

รายการนี้ไม่ได้หมายถึงให้หยุดทำทุกอย่าง
ให้แยก feature ที่ blocked ออก แล้วพัฒนา Firmware ส่วนที่ข้อมูลครบพร้อมรายงาน assumptions ที่ใช้

## 16. สิ่งที่ต้องส่งมอบจาก Claude

1. Requirements matrix แยก HW/REQ/PROPOSED/VERIFY และ hardware blockers
2. Final board pin map และ power domain/polarity table ที่อิง PDF
3. Architecture และ state/event flow
4. โครงสร้าง ESP-IDF project สำหรับ ESP32-S3-WROOM-1U-N16R8 พร้อม partitions.csv/custom log backend และรายงานผล build จริง
5. board config รวม GPIO/address/active levels ไว้ที่เดียว
6. drivers/services สำหรับอุปกรณ์ที่มีข้อมูลครบ; TODO/feature flag เฉพาะส่วนที่ยังขาด ไม่แต่ง register
7. Trip/log/config/protocol schema, binary framing, capacity budget 31 วัน, whole-Trip retention/SD migration และ command documentation
8. USB MSC virtual FAT/CSV snapshot ที่ใช้ได้โดยไม่มี SD + BLE GATT/NFC NDEF + optional CDC protocol ที่ compatible กัน
9. Default configuration พร้อมที่มาของค่าและขอบเขตการปรับ
10. Power budget, charging policy และวิธีวัด runtime
11. Bring-up/test plan พร้อม critical acceptance cases
12. README วิธี build/flash/debug/recover และ known limitations
13. หากทำ PC test script ให้รองรับเลือก Device ID และหลาย COM ports โดยไม่จำเป็นต้องทำ UI app เต็มก่อน
14. Wi-Fi live/backlog uploader พร้อม application ACK/idempotency และ server contract ที่เสนอ
15. Host-side capacity/power-cut/retention/CSV consistency checks พร้อมผลทดสอบที่ทำจริง
16. รายงานให้ชัดว่าส่วนใด build/test จริงแล้ว และส่วนใดยังรอ hardware/server

ให้เริ่มคำตอบด้วย:

- สรุปสิ่งที่เข้าใจจาก revision นี้
- แสดง critical blockers ที่กระทบการเขียนโค้ดจริง โดยเฉพาะ PD/e-paper/Dock identity
- แสดง GPIO map รวม GNSS TX17/RX18 และ LED_PWR_EN active LOW
- เสนอ software architecture/project tree รวม Flash primary, optional SD, Wi-Fi sync และ USB MSC/CSV
- จากนั้นเริ่มสร้างโค้ดส่วน bring-up/core ที่ข้อมูลครบต่อทันที

อย่าเขียนเพียง pseudocode ทั้งโปรเจกต์ถ้าส่วนใดสามารถทำจริงได้
อย่าอ้างว่าแก้ hardware ด้วย Firmware ได้ หรืออ้างว่าผ่านการทดสอบที่ยังไม่ได้ทำ
