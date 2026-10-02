/**
 * @file board_esp32_st7789_diy.h
 * Board definition: ESP-WROOM32 (ESP32 classic) + ST7789 2.8" 240x320 + XPT2046 touch
 *
 * MCU:      ESP32 (dual-core LX6, classic WROOM32, 4MB flash, NO PSRAM)
 * Display:  ST7789 2.8" 240x320 RGB565 via VSPI (23/19/18), RGB-BGR order
 * Touch:    XPT2046 resistive touch on the same SPI bus (CS 21, shared 23/19/18)
 * Input:    XPT2046 touch d-pad (zones + hold-to-Back) + BOOT button (GPIO0)
 * SD Card:  SPI (shared bus with LCD, CS 12)
 * SubGHz:   CC1101 optional via shared VSPI (CS 15, same as ili9341 build)
 * NRF24:    optional via shared VSPI (CS 15 / CE 4, same as ili9341 build;
 *           use EITHER CC1101 OR NRF24)
 * IR:       none in v1 wiring
 * I2C:      Grove sensors on SCL 22 / SDA 25 (see I2C note below)
 * LED:      single status LED on GPIO2 (active-high)
 *
 * SPI bus architecture (all share MOSI=23 / MISO=19 / SCK=18, own CS each):
 *   Display (CS=17) — ST7789 + BL(32)
 *   Touchscreen (CS=21) — XPT2046 (IRQ not connected, polling)
 *   SD Card (CS=12) — microSD slot
 *   Radio (CS=15) — NRF24 or CC1101
 *
 * Wiring (from board spec):
 *   Display: CS=17, RESET=5, DC=16, MOSI=23, MISO=19, SCK=18, BL=32
 * Touch:   MOSI=23, MISO=19, SCK=18, CS=21, IRQ=N/C
 *   SD:      MOSI=23, MISO=19, SCK=18, CS=12 (same as ili9341 build)
 *   Radio:   MOSI=23, MISO=19, SCK=18, CS=15, CE=4 (same as ili9341 build)
 *   Button:  BOOT=0 (active-LOW, remapped as OK; no 5-way pad on this build)
 *   LED:     anode=2 (active-HIGH)
 *   I2C:     SCL=22, SDA=21(*)
 *   Serial:  RX=3, TX=1 (UART0, programming/debug)
 *
 * (*) I2C CONFLICT — READ THIS: the wiring table lists I2C SDA on GPIO21,
 *     but GPIO21 is also the XPT2046 touch CS. Both cannot share one pin:
 *     touch CS idles HIGH / asserts LOW, I2C SDA is open-drain with pull-up —
 *     they corrupt each other (touch reads break I2C, I2C traffic de-selects
 *     the touch controller mid-transfer). Firmware resolves it by moving the
 *     sensor bus SDA to GPIO25 (free, non-strapping, input-capable):
 *       BOARD_PIN_QWIIC_SDA = 25, BOARD_PIN_QWIIC_SCL = 22
 *     Wire Grove sensors to 25/22. If your hardware hard-wires SDA to 21,
 *     either move touch CS to a free GPIO (e.g. 25) and update
 *     BOARD_PIN_TOUCH_CS below, or leave I2C sensors unconnected.
 *
 * Radio GDO pins: kept identical to the ili9341 build (verified in
 * board_esp32_ili9341_diy.h: CSN=15, GDO0=4, GDO2=13) so the same NRF24/CC1101
 * module wiring works on both firmwares.
 *
 * LIMITATIONS (same class as esp32_ili9341_diy — read before use):
 *  - Classic ESP32 has ~520KB SRAM, no PSRAM on WROOM32, 4MB flash.
 *    This firmware was designed for S3+8MB PSRAM+16MB flash. Expect OOM in
 *    WiFi/BLE/SubGHz heavy paths. BLE is OFF (BOARD_HAS_BLE=0): the BT
 *    controller + Bluedroid eat ~100KB DRAM, which pushes Sub-GHz/WiFi over
 *    the edge on this target. WiFi still works. Streaming app is excluded
 *    via fam_config.py for the same reason.
 *  - No native USB-OTG on classic ESP32. BadUSB-USB / qFlipper-USB /
 *    USB-Storage are stubbed (furi_hal_usb_hid_stub). Use UART0 + WiFi only.
 *  - Logic 3.3V, ~600mA typical, ~1000mA peak (radio TX + BL full).
 *    Use a supply rated >= 1.5A.
 */

#pragma once

/* ---- Board metadata ---- */
#define BOARD_NAME        "ESP32 ST7789 DIY"
#define BOARD_ID          "esp32_st7789_diy"
#define BOARD_TARGET      "esp32"

/* ---- Hardware Button Pins ----
 * Only the BOOT button exists in v1 wiring (active-low). It is remapped as
 * OK (single-click) / Back (double-click) by target_input.c. The UP/DOWN/
 * LEFT/RIGHT/BACK defines below are for the OPTIONAL 6-button pad
 * (BOARD_HAS_BUTTONS_PAD=1); they are unused while the pad is off but must
 * still be valid classic-ESP32 GPIOs. All are free in v1 wiring. */
#define BOARD_PIN_BTN_UP        33
#define BOARD_PIN_BTN_DOWN      26
#define BOARD_PIN_BTN_LEFT      27
#define BOARD_PIN_BTN_RIGHT     14
#define BOARD_PIN_BTN_OK        0   /* BOOT button, remapped as OK */
#define BOARD_PIN_BTN_BACK      35  /* optional pad only (input-only, ext pull-up req.) */
#define BOARD_PIN_BUTTON_BOOT   0
#define BOARD_HAS_BUTTONS_PAD   0   /* 1 = also poll the 6-button pad above */
#define BOARD_PIN_BATTERY_ADC   UINT16_MAX  /* No battery ADC on this board */

/* ---- LCD Pins (ST7789 2.8" 240x320 via VSPI pins, driven as SPI2_HOST) ----
 * NOTE: VSPI pins 23/19/18 are routed through SPI2_HOST via the GPIO matrix
 * so LCD + touch + SD + radio share one bus (furi_hal_spi.c / furi_hal_sd.c
 * hardcode SPI2_HOST for the shared bus). Do NOT change to SPI3_HOST. */
#define BOARD_PIN_LCD_MOSI      23
#define BOARD_PIN_LCD_MISO      19  /* shared with touch/SD/radio MISO */
#define BOARD_PIN_LCD_SCLK      18
#define BOARD_PIN_LCD_DC        16
#define BOARD_PIN_LCD_CS        17
#define BOARD_PIN_LCD_RST       5
#define BOARD_PIN_LCD_BL        32  /* Backlight PWM, active-high */

/* ---- LCD Display Configuration (portrait 240x320, ST7789) ----
 * The 2.8" panel is natively 240x320 portrait. The Flipper UI is a wide 2:1
 * layout, so it reads far better turned 90°. Landscape = rotation 1 or 3
 * (180° apart). Rotation 3 is upright on this module class (1 = upside-down).
 * Settings → Interface → Screen Rotation changes it live (a saved value wins).
 * If your module mounts the ribbon the other way, flip BOARD_LCD_MIRROR_X /
 * _Y. If orange looks blue, flip BOARD_LCD_COLOR_ORDER_BGR. */
#define BOARD_LCD_H_RES         240
#define BOARD_LCD_V_RES         320
#define BOARD_LCD_SPI_HOST      SPI2_HOST
#define BOARD_LCD_SPI_FREQ_HZ   (20 * 1000 * 1000)  /* 20MHz conservative on long Dupont wires */
#define BOARD_LCD_CMD_BITS      8
#define BOARD_LCD_PARAM_BITS    8
#define BOARD_LCD_SWAP_XY       true
#define BOARD_LCD_MIRROR_X      false
#define BOARD_LCD_MIRROR_Y      false
#define BOARD_LCD_DEFAULT_ROTATION 3    /* landscape upright (1 = upside-down) */
#define BOARD_LCD_INVERT_COLOR  false   /* INVON off: with it on this panel class renders a negative */
#define BOARD_LCD_GAP_X         0
#define BOARD_LCD_GAP_Y         0
#define BOARD_LCD_BL_ACTIVE_LOW false   /* Active-high backlight */
#define BOARD_LCD_COLOR_ORDER_BGR false /* RGB order (MADCTL BGR bit off): with BGR on,
                                           * orange renders sky-blue (R/B swapped) — verified
                                           * on hardware. Same as the S3 ST7789 DIY panel. */

/* Flipper framebuffer -> display color mapping (RGB565, native byte order) */
#define BOARD_LCD_FG_COLOR      0xA0FD  /* Flipper Orange */
#define BOARD_LCD_BG_COLOR      0x0000  /* Black */

/* ---- SD Card (module slot, shared VSPI bus, CS=12 like the ili9341 build) ----
 * Without a card the init just fails softly (BOARD_SD_OPTIONAL=1 keeps the
 * UI quiet about it). */
#define BOARD_PIN_SD_CS         12
#define BOARD_PIN_SD_MISO       19

/* ---- Touchscreen: XPT2046 resistive, SPI (shares VSPI with LCD/SD/radio) ----
 * IRQ is not connected in this wiring — coordinates are read by polling T_CS.
 * Touch CS=21 per wiring. See the I2C conflict note at the top of this file. */
#define BOARD_TOUCH_XPT2046     1
#define BOARD_PIN_TOUCH_CS      21      /* XPT2046 chip-select (T_CS) */
#define BOARD_PIN_TOUCH_MOSI    23      /* shared with LCD_MOSI / radio MOSI / SD MOSI */
#define BOARD_PIN_TOUCH_MISO    19      /* shared with LCD_MISO / radio MISO / SD MISO */
#define BOARD_PIN_TOUCH_CLK     18      /* shared with LCD_CLK / radio SCK / SD SCK */
#define BOARD_TOUCH_SPI_HOST    SPI2_HOST
#define BOARD_TOUCH_SPI_FREQ_HZ (2 * 1000 * 1000) /* XPT2046 max ~2 MHz */

/* Raw window (12-bit, 0..4095) that counts as a real finger. Samples outside
 * this window, or noisy samples, are treated as "not pressed". Panels differ,
 * so widen/tighten these two pairs if edge touches are dropped. */
#define BOARD_TOUCH_RAW_X_MIN   200
#define BOARD_TOUCH_RAW_X_MAX   3900
#define BOARD_TOUCH_RAW_Y_MIN   200
#define BOARD_TOUCH_RAW_Y_MAX   3900
#define BOARD_TOUCH_RAW_TOLERANCE 400
/* Median sampling (15 per axis) for this panel: its light-touch readings are
 * noisier than the S3 unit's, and 2-sample agreement rejected them (hard
 * press required). S3 and all other boards keep the 2-sample path. */
#define BOARD_TOUCH_MEDIAN_SAMPLES 15

/* Axis orientation. The touch panel is natively portrait like the LCD, so in
 * landscape the touch axes swap as well. Display rotation 3 sets MY on the
 * panel, so touch Y must mirror too, else down scrolls up. If left/right
 * come out swapped, set MIRROR_X. */
#define BOARD_TOUCH_SWAP_XY     true
#define BOARD_TOUCH_MIRROR_X    true    /* verified on hardware: with false, left/right come out swapped */
#define BOARD_TOUCH_MIRROR_Y    true
/* Touch output space stays landscape 320x240 on purpose: the Flipper UI runs
 * landscape (rotation 1/3), so touch coordinates must match the UI, not the
 * panel's portrait mounting. Do NOT tie these to BOARD_LCD_* (portrait). */
#define BOARD_TOUCH_H_RES       320
#define BOARD_TOUCH_V_RES       240

/* Unused I2C touch pins (kept so furi_hal_resources.c stays board-agnostic) */
#define BOARD_PIN_TOUCH_SCL     UINT16_MAX
#define BOARD_PIN_TOUCH_SDA     UINT16_MAX
#define BOARD_PIN_TOUCH_RST     UINT16_MAX
#define BOARD_PIN_TOUCH_INT     UINT16_MAX
#define BOARD_TOUCH_I2C_ADDR    0x00
#define BOARD_TOUCH_I2C_PORT    I2C_NUM_0
#define BOARD_TOUCH_I2C_FREQ_HZ 0
#define BOARD_TOUCH_I2C_TIMEOUT 0

/* ---- SubGHz / CC1101 (shared VSPI bus with LCD + touch + SD) ----
 * Pinout mirrors the ili9341 build (verified: SD=12, CSN=15, GDO0=4, GDO2=13)
 * so the same module wiring just works. */
#define BOARD_PIN_CC1101_SCK    18      /* Shared SCK */
#define BOARD_PIN_CC1101_CSN    15      /* Own CS, isolated from LCD_CS=17 / T_CS=21 / SD_CS=12 */
#define BOARD_PIN_CC1101_MISO   19      /* Shared MISO */
#define BOARD_PIN_CC1101_MOSI   23      /* Shared MOSI */
#define BOARD_PIN_CC1101_GDO0   4       /* Same as ili9341 build */
#define BOARD_PIN_CC1101_GDO2   13      /* Same as ili9341 build */
#define BOARD_CC1101_SPI_SHARED 1

/* ---- NRF24L01 (optional, same VSPI — use EITHER CC1101 OR NRF24, not both) ----
 * Pinout mirrors the ili9341 build (verified working with the same module). */
#define BOARD_PIN_NRF24_SCK     BOARD_PIN_CC1101_SCK
#define BOARD_PIN_NRF24_MISO    BOARD_PIN_CC1101_MISO
#define BOARD_PIN_NRF24_MOSI    BOARD_PIN_CC1101_MOSI
#define BOARD_PIN_NRF24_CSN     15      /* shares radio CS footprint */
#define BOARD_PIN_NRF24_CE      4       /* shares GDO0 footprint, output */
#define BOARD_HAS_NRF24         1

/* ---- IR (not in v1 wiring) ---- */
#define BOARD_PIN_IR_TX         UINT16_MAX
#define BOARD_PIN_IR_RX         UINT16_MAX

/* ---- NFC / PN532 (not in v1 wiring, Grove I2C reserved) ----
 * SDA is on GPIO25, NOT 21: GPIO21 is the touch CS (see conflict note). */
#define BOARD_PIN_NFC_SCL       22
#define BOARD_PIN_NFC_SDA       25
#define BOARD_PIN_NFC_IRQ       UINT16_MAX
#define BOARD_PIN_NFC_RST       UINT16_MAX
#define BOARD_NFC_I2C_PORT      I2C_NUM_0

/* ---- Status LED (single, active-high) ---- */
#define BOARD_PIN_STATUS_LED    2

/* ---- Unused audio/RGB (not in wiring) ---- */
#define BOARD_PIN_SPEAKER_BCLK  -1
#define BOARD_PIN_SPEAKER_WCLK  -1
#define BOARD_PIN_SPEAKER_DOUT  -1
#define BOARD_PIN_WS2812_DATA   UINT16_MAX
#define BOARD_WS2812_LED_COUNT  0
#define BOARD_PIN_MIC_DATA      UINT16_MAX
#define BOARD_PIN_MIC_CLK       UINT16_MAX

/* ---- Qwiic / Grove I2C (SDA moved to 25 — see conflict note at top) ---- */
#define BOARD_PIN_QWIIC_SDA     25
#define BOARD_PIN_QWIIC_SCL     22

/* ---- Features (conservative for 4MB/no-PSRAM) ---- */
#define BOARD_HAS_TOUCH         1   /* XPT2046 resistive (SPI) */
#define BOARD_HAS_ENCODER       0
#define BOARD_HAS_SD_CARD       1
/* The SD slot is optional: when no card is present the desktop stays usable
 * instead of blocking on a "bad SD" prompt. Set to 0 for the usual Flipper
 * "insert SD card" behaviour. */
#define BOARD_SD_OPTIONAL       1
/* BLE off: classic ESP32 has no PSRAM — the BT controller + Bluedroid eat
 * ~100KB of the ~320KB DRAM, which pushes Sub-GHz/WiFi over the edge
 * (StoreProhibited on launch). Same tradeoff as esp32_ili9341_diy. WiFi
 * still works. Flip back to 1 if you need BLE and can afford the RAM. */
#define BOARD_HAS_BLE           0
#define BOARD_HAS_RGB_LED       0
#define BOARD_HAS_VIBRO         0
#define BOARD_HAS_SPEAKER       0
#define BOARD_HAS_IR            0
#define BOARD_HAS_IBUTTON       0
#define BOARD_HAS_RFID          0
#define BOARD_HAS_NFC           0   /* set to 1 once PN532 is wired + tested */
#define BOARD_HAS_SUBGHZ        1
#define BOARD_HAS_MIC           0

/* Power service fallbacks (no fuel gauge/charger on DIY board) */
#define BQ27220_ADDR                        0x55
#define BQ25896_CHARGE_LIMIT                1280
#define FURI_HAL_POWER_VIRTUAL_CAPACITY_MAH (1300U)
