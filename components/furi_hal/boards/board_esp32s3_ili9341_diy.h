/**
 * @file board_esp32s3_ili9341_diy.h
 * Board definition: Generic ESP32-S3 N16R8 + ILI9341 2.4"/2.8" 240x320 + XPT2046 touch
 *
 * MCU:      ESP32-S3 N16R8 (16MB Flash + 8MB Octal PSRAM)
 * Display:  ILI9341 240x320 RGB565 via SPI (same wiring as S3 ST7789 DIY build)
 * Touch:    XPT2046 resistive touch on the same SPI bus (CS 3, shared 11/12/13)
 * Input:    XPT2046 touch d-pad (zones + hold-to-Back) + BOOT button
 * SD Card:  SPI (shared bus with LCD, CS 4)
 * SubGHz:   CC1101 optional via shared SPI (CS 15)
 * NRF24:    optional via shared SPI (CS 14 / CE 16)
 * IR:       TX GPIO2 + RX GPIO1 (same as S3 ST7789 DIY)
 * NFC:      disabled (no PN532 in wiring, I2C 8/9 reserved for future)
 *
 * Wiring is IDENTICAL to esp32s3_st7789_diy — only the panel controller
 * differs (ILI9341 instead of ST7789), so the same module wiring works:
 *   MOSI=11  MISO=13  SCLK=12   LCD_CS=10  LCD_DC=7  LCD_RST=5  BL=38  T_CS=3
 *
 * Both 2.4" and 2.8" ILI9341 modules are 240x320 — one firmware covers both.
 * The only difference is physical size; the driver init is identical.
 *
 * ILI9341 notes (from esp32_ili9341_diy, verified on hardware):
 *  - MX must be set on these ILI9341 modules: with it clear the panel renders
 *    mirrored (glyphs flipped). 0x48 = MX|BGR is the canonical portrait value.
 *  - Most 2.4"/2.8" ILI9341 modules are BGR colour order.
 *  - Typically non-inverted (INVON off).
 */

#pragma once

/* ---- Board metadata ---- */
#define BOARD_NAME        "ESP32-S3 ILI9341 DIY"
#define BOARD_ID          "esp32s3_ili9341_diy"
#define BOARD_TARGET      "esp32s3"

/* ---- Hardware Button Pins (same as S3 ST7789 DIY) ---- */
#define BOARD_PIN_BTN_UP        42
#define BOARD_PIN_BTN_DOWN      47
#define BOARD_PIN_BTN_LEFT      48
#define BOARD_PIN_BTN_RIGHT     18
#define BOARD_PIN_BTN_OK        0   /* BOOT button, remapped as OK */
#define BOARD_PIN_BTN_BACK      41
#define BOARD_PIN_BUTTON_BOOT   0
#define BOARD_HAS_BUTTONS_PAD   0
#define BOARD_PIN_BATTERY_ADC   UINT16_MAX

/* ---- LCD Pins (ILI9341 240x320 via SPI, same wiring as S3 ST7789 DIY) ---- */
#define BOARD_PIN_LCD_MOSI      11
#define BOARD_PIN_LCD_SCLK      12
#define BOARD_PIN_LCD_DC        7
#define BOARD_PIN_LCD_CS        10
#define BOARD_PIN_LCD_RST       5
#define BOARD_PIN_LCD_BL        38

/* ---- LCD Display Configuration (portrait 240x320, ILI9341) ----
 * Panel is natively 240x320 portrait, Flipper UI runs landscape (rotation 3).
 * ILI9341-specific bits (MX + BGR) come from the verified classic ILI9341 build;
 * geometry/rotation handling matches the S3 ST7789 DIY build. */
#define BOARD_DISPLAY_ILI9341   1   /* switches furi_hal_display.c to ili9341 panel */
#define BOARD_LCD_H_RES         240
#define BOARD_LCD_V_RES         320
#define BOARD_LCD_SPI_HOST      SPI2_HOST
#define BOARD_LCD_SPI_FREQ_HZ   (40 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS      8
#define BOARD_LCD_PARAM_BITS    8
#define BOARD_LCD_SWAP_XY       true
#define BOARD_LCD_MIRROR_X      true   /* ILI9341 2.4"/2.8" need MX (verified on classic build) */
#define BOARD_LCD_MIRROR_Y      false
#define BOARD_LCD_DEFAULT_ROTATION 3    /* landscape upright (1 = upside-down) */
#define BOARD_LCD_INVERT_COLOR  false   /* ILI9341 2.4"/2.8" typically non-inverted */
#define BOARD_LCD_GAP_X         0
#define BOARD_LCD_GAP_Y         0
#define BOARD_LCD_BL_ACTIVE_LOW false
#define BOARD_LCD_COLOR_ORDER_BGR true  /* most 2.4"/2.8" ILI9341 modules are BGR */

/* Flipper framebuffer -> display color mapping (RGB565, native byte order) */
#define BOARD_LCD_FG_COLOR      0xA0FD  /* Flipper Orange */
#define BOARD_LCD_BG_COLOR      0x0000  /* Black */

/* ---- SD Card Pins (same as S3 ST7789 DIY) ---- */
#define BOARD_PIN_SD_CS         4
#define BOARD_PIN_SD_MISO       13

/* ---- Touchscreen: XPT2046 resistive, SPI (shares bus with LCD) ---- */
#define BOARD_TOUCH_XPT2046     1
#define BOARD_PIN_TOUCH_CS      3
#define BOARD_PIN_TOUCH_MOSI    11
#define BOARD_PIN_TOUCH_MISO    13
#define BOARD_PIN_TOUCH_CLK     12
#define BOARD_TOUCH_SPI_HOST    SPI2_HOST
#define BOARD_TOUCH_SPI_FREQ_HZ (2 * 1000 * 1000)

#define BOARD_TOUCH_RAW_X_MIN   200
#define BOARD_TOUCH_RAW_X_MAX   3900
#define BOARD_TOUCH_RAW_Y_MIN   200
#define BOARD_TOUCH_RAW_Y_MAX   3900
#define BOARD_TOUCH_RAW_TOLERANCE 400

#define BOARD_TOUCH_SWAP_XY     true
#define BOARD_TOUCH_MIRROR_X    true    /* verified on S3 ST7789 hardware */
#define BOARD_TOUCH_MIRROR_Y    true
#define BOARD_TOUCH_H_RES       320
#define BOARD_TOUCH_V_RES       240

/* Unused I2C touch pins */
#define BOARD_PIN_TOUCH_SCL     UINT16_MAX
#define BOARD_PIN_TOUCH_SDA     UINT16_MAX
#define BOARD_PIN_TOUCH_RST     UINT16_MAX
#define BOARD_PIN_TOUCH_INT     UINT16_MAX
#define BOARD_TOUCH_I2C_ADDR    0x00
#define BOARD_TOUCH_I2C_PORT    I2C_NUM_0
#define BOARD_TOUCH_I2C_FREQ_HZ 0
#define BOARD_TOUCH_I2C_TIMEOUT 0

/* ---- SubGHz / CC1101 (shared SPI bus) ---- */
#define BOARD_PIN_CC1101_SCK    12
#define BOARD_PIN_CC1101_CSN    15
#define BOARD_PIN_CC1101_MISO   13
#define BOARD_PIN_CC1101_MOSI   11
#define BOARD_PIN_CC1101_GDO0   6
#define BOARD_PIN_CC1101_GDO2   UINT16_MAX
#define BOARD_CC1101_SPI_SHARED 1

/* ---- NRF24L01 (optional, shared SPI bus) ---- */
#define BOARD_PIN_NRF24_SCK     BOARD_PIN_CC1101_SCK
#define BOARD_PIN_NRF24_MISO    BOARD_PIN_CC1101_MISO
#define BOARD_PIN_NRF24_MOSI    BOARD_PIN_CC1101_MOSI
#define BOARD_PIN_NRF24_CSN     14
#define BOARD_PIN_NRF24_CE      16
#define BOARD_HAS_NRF24         1

/* ---- IR (same as S3 ST7789 DIY) ---- */
#define BOARD_PIN_IR_TX         2
#define BOARD_PIN_IR_RX         1

/* ---- NFC / PN532 (reserved, disabled) ---- */
#define BOARD_PIN_NFC_SCL       9
#define BOARD_PIN_NFC_SDA       8
#define BOARD_PIN_NFC_IRQ       UINT16_MAX
#define BOARD_PIN_NFC_RST       UINT16_MAX
#define BOARD_NFC_I2C_PORT      I2C_NUM_0

/* ---- Status LED ---- */
#define BOARD_PIN_STATUS_LED    40

/* ---- Unused audio/RGB ---- */
#define BOARD_PIN_SPEAKER_BCLK  -1
#define BOARD_PIN_SPEAKER_WCLK  -1
#define BOARD_PIN_SPEAKER_DOUT  -1
#define BOARD_PIN_WS2812_DATA   UINT16_MAX
#define BOARD_WS2812_LED_COUNT  0
#define BOARD_PIN_MIC_DATA      UINT16_MAX
#define BOARD_PIN_MIC_CLK       UINT16_MAX

/* ---- Qwiic / External I2C ---- */
#define BOARD_PIN_QWIIC_SDA     8
#define BOARD_PIN_QWIIC_SCL     9

/* ---- Features (same as S3 ST7789 DIY — full PSRAM/S3 feature set) ---- */
#define BOARD_HAS_TOUCH         1
#define BOARD_HAS_ENCODER       0
#define BOARD_HAS_SD_CARD       1
#define BOARD_SD_OPTIONAL       1
#define BOARD_HAS_BLE           1
#define BOARD_HAS_RGB_LED       0
#define BOARD_HAS_VIBRO         0
#define BOARD_HAS_SPEAKER       0
#define BOARD_HAS_IR            1
#define BOARD_HAS_IBUTTON       0
#define BOARD_HAS_RFID          0
#define BOARD_HAS_NFC           0
#define BOARD_HAS_SUBGHZ        1
#define BOARD_HAS_MIC           0

/* Power service fallbacks */
#define BQ27220_ADDR                        0x55
#define BQ25896_CHARGE_LIMIT                1280
#define FURI_HAL_POWER_VIRTUAL_CAPACITY_MAH (1300U)
