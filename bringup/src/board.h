// mCOLD Foam V.1 pin map, transcribed from the GPIO table in
// docs/mCOLD_Firmware_Requirements_2026-09-19.md.
//
// Nothing here is guessed. Where the requirement document flags something
// as needing confirmation on real hardware, the comment says so, and the
// bring-up command that touches it reports what it actually found rather
// than assuming the document is right.
#pragma once

// ---- I2C: one shared bus, seven devices ----------------------------
#define PIN_SDA        15
#define PIN_SCL        16

// ---- SPI3: e-paper and the thermocouple front end ------------------
#define PIN_SPI3_SCK   40
#define PIN_SPI3_MOSI  41
#define PIN_SPI3_MISO  39
#define PIN_EPD_CS     38
#define PIN_EPD_DC      8
#define PIN_EPD_RST     9     // 10k pull-up to the panel rail
#define PIN_EPD_BUSY   21     // BUSY_N per the panel doc: LOW busy, HIGH ready
#define PIN_TC_CS      14     // MAX6675

// ---- SPI2: microSD -------------------------------------------------
#define PIN_SPI2_CLK   12
#define PIN_SPI2_MOSI  11
#define PIN_SPI2_MISO  13
#define PIN_SD_CS      10

// ---- GNSS UART -----------------------------------------------------
// Named from the module's side in the schematic. The MCU transmits on 17
// and receives on 18; the requirement document warns in as many words not
// to set RX=17 by reading the net name alone.
#define PIN_GNSS_TX    17     // MCU TX  -> CN1 RXD0
#define PIN_GNSS_RX    18     // MCU RX  <- CN1 TXD0

// ---- Rails ---------------------------------------------------------
// Every one of these boots to its OFF state. A bring-up that powers
// everything at once cannot tell you which load is the one pulling the
// rail down.
#define PIN_EPD_PWR_EN 42     // HIGH = on   (e-paper + MAX6675)
#define PIN_SD_PWR_EN  47     // HIGH = on   (microSD + GNSS, one switch)
#define PIN_LED_PWR_EN 48     // LOW  = on   (P-MOS gate)

// ---- Addressable LEDs ----------------------------------------------
// GPIO43 is the ROM console TX pad. The application must not open UART0
// on it, and the boot ROM's own output will twitch the data line before
// setup() runs -- harmless, but it is why the first pixel sometimes shows
// a colour until the first write.
#define PIN_LED_DATA   43
#define LED_COUNT       4     // index 0 is LED4, the side charge light

// ---- Charger -------------------------------------------------------
#define PIN_CHG_CE_N   44     // LOW allows charging, HIGH disables it

// ---- Discrete IO ---------------------------------------------------
#define PIN_BUZZER      6     // HIGH drives Q5
#define PIN_DOOR        7     // 1M pull-up + 47n; polarity is a config item
#define PIN_NFC_GPO     1     // ST25DV event out
#define PIN_ACC_INT1    2     // LIS2DW12 INT1
#define PIN_PG_N        4     // BQ25601 PG#, LOW = power good
#define PIN_PMIC_IRQ_N  5     // BQ25601 INT# and HUSB238A INT_N, wired together

// ---- I2C addresses, 7-bit ------------------------------------------
#define ADDR_LIS2DW12  0x18
#define ADDR_MAX17048  0x36
#define ADDR_INA226    0x40
#define ADDR_HUSB238A  0x42
#define ADDR_ST25_USER 0x53
#define ADDR_ST25_SYS  0x57   // the same chip, not an eighth device
#define ADDR_PCF8523   0x68
#define ADDR_BQ25601   0x6B
