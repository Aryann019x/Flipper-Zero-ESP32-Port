/**
 * @file board_esp32s3_st7789_diy.h
 * Board definition: Generic ESP32-S3 N16R8 + ST7789 2.8" 240x320 + XPT2046 touch
 *
 * MCU:      ESP32-S3 N16R8 (16MB Flash + 8MB Octal PSRAM)
 * Display:  ST7789 240x320 RGB565 via SPI (HSPI pins 11/12/13)
 * Touch:    XPT2046 resistive touch on the same SPI bus (CS 3, shared 11/12/13)
 * Input:    XPT2046 touch d-pad (zones + hold-to-Back) + BOOT button
 * SD Card:  SPI (shared bus with LCD)
 * SubGHz:   CC1101 optional via shared SPI (CS fixed to GPIO15, NOT 10)
 * NRF24:    optional via shared SPI (CS 14 / CE 16)
 * IR:       TX GPIO2 (IR_OUT emitter) + RX GPIO1 (IR_SIG receiver)
 * NFC:      disabled (no PN532 in wiring, I2C 8/9 reserved for future)
 *
 * This board mirrors the common "ESP32-S3 + 2.8\" ST7789 + XPT2046" DIY build
 * (Bruce / Marauder style wiring). Display and touch share HSPI:
 *   MOSI=11  MISO=13  SCLK=12   LCD_CS=10   T_CS=3
 * The XPT2046 has no hardware gesture engine, so target_input.c treats the
 * panel as a positional d-pad plus a direct-touch bottom button bar:
 * bottom-left = Left, bottom-center = OK, bottom-right = Right (tap where
 * the on-screen buttons are), top-left corner = Back, middle hold = Back.
 * BOOT stays as single-click = OK, double-click = Back.
 *
 * Optional 6-button pad (BOARD_HAS_BUTTONS_PAD): the original wiring doc only
 * has the BOOT button, so the pad is OFF by default. Set it to 1 and wire the
 * BOARD_PIN_BTN_* pins if you prefer physical navigation; the touch panel can
 * then be left unconnected.
 *
 * Notes vs. the original wiring doc:
 *  - CC1101 CS was 10 = Display CS 10 -> moved to 15 (shared bus)
 *  - CC1101 GDO0 was 9 = I2C SCL 9 -> moved to 6
 *  - Touch CS 3 is a strapping pin; it is driven as a normal CS after boot,
 *    which is fine because the ESP32-S3 only samples it during reset.
 *  - GPIO33-37 are reserved for Octal Flash/PSRAM, 19/20 are USB -> avoided
 */

#pragma once

/* ---- Board metadata ---- */
#define BOARD_NAME        "ESP32-S3 ST7789 DIY"
#define BOARD_ID          "esp32s3_st7789_diy"
#define BOARD_TARGET      "esp32s3"

/* ---- Hardware Button Pins (6x tactile, active-low) ---- */
#define BOARD_PIN_BTN_UP        42  /* optional pad (BOARD_HAS_BUTTONS_PAD) */
#define BOARD_PIN_BTN_DOWN      47
#define BOARD_PIN_BTN_LEFT      48
#define BOARD_PIN_BTN_RIGHT     18
#define BOARD_PIN_BTN_OK        0   /* BOOT button, remapped as OK */
#define BOARD_PIN_BTN_BACK      41
#define BOARD_PIN_BUTTON_BOOT   0
#define BOARD_HAS_BUTTONS_PAD   0   /* 1 = also poll the 6-button pad above */
#define BOARD_PIN_BATTERY_ADC   UINT16_MAX  /* No battery ADC on this board */

/* ---- LCD Pins (ST7789 240x320 via SPI) ---- */
#define BOARD_PIN_LCD_MOSI      11
#define BOARD_PIN_LCD_SCLK      12
#define BOARD_PIN_LCD_DC        7
#define BOARD_PIN_LCD_CS        10
#define BOARD_PIN_LCD_RST       5
#define BOARD_PIN_LCD_BL        38  /* Backlight PWM */

/* ---- LCD Display Configuration (portrait 240x320) ----
 * The 2.8" panel is natively 240x320 portrait. The Flipper UI is a wide 2:1
 * layout, so it reads far better turned 90°. Landscape = rotation 1 or 3
 * (180° apart). Rotation 1 renders upside-down on this module class, so the
 * default is 3 = upright. Settings → Interface → Screen Rotation changes it
 * live (a saved value wins).
 * If your module mounts the ribbon the other way, flip BOARD_LCD_MIRROR_X /
 * _Y. */
#define BOARD_LCD_H_RES         240
#define BOARD_LCD_V_RES         320
#define BOARD_LCD_SPI_HOST      SPI2_HOST
#define BOARD_LCD_SPI_FREQ_HZ   (40 * 1000 * 1000)
#define BOARD_LCD_CMD_BITS      8
#define BOARD_LCD_PARAM_BITS    8
#define BOARD_LCD_SWAP_XY       true
#define BOARD_LCD_MIRROR_X      false   /* MX on mirrors L-R on this panel (verified) */
#define BOARD_LCD_MIRROR_Y      false
#define BOARD_LCD_DEFAULT_ROTATION 3    /* landscape upright (1 = upside-down) */
/* This 2.8" module wants INVON *off*. With it on, every pixel comes out as the
 * photographic negative: the White "UI Background" preset renders black, Green
 * text renders magenta/purple and the Purple preset renders chartreuse-green
 * (0x881F -> 0x77E0, i.e. R/G ≈ 0.45 — a straight R/B swap would not do that).
 * Verified on hardware; flip back to true only if a differently-sourced module
 * of the same size turns out inverted. */
#define BOARD_LCD_INVERT_COLOR  false
#define BOARD_LCD_GAP_X         0
#define BOARD_LCD_GAP_Y         0
#define BOARD_LCD_BL_ACTIVE_LOW false   /* Active-high backlight */
#define BOARD_LCD_COLOR_ORDER_BGR false /* RGB order (MADCTL BGR bit off), same as the
                                          * working T-Embed ST7789: with BGR on, every
                                          * colour came out with R and B swapped —
                                          * orange looked blue, purple came out rose. */

/* Flipper framebuffer -> display color mapping (RGB565, native byte order) */
#define BOARD_LCD_FG_COLOR      0xA0FD  /* Flipper Orange */
#define BOARD_LCD_BG_COLOR      0x0000  /* Black */

/* ---- SD Card Pins (LCD module's built-in micro-SD slot, shared SPI bus) ----
 * The 2.8" ST7789 module exposes SD_CS on GPIO4 and shares the LCD's MISO=13.
 * GPIO4 is NOT free for anything else. */
#define BOARD_PIN_SD_CS         4
#define BOARD_PIN_SD_MISO       13

/* ---- Touchscreen: XPT2046 resistive, SPI (shares HSPI with the LCD) ----
 * The touch controller has no reset or interrupt line in this wiring, so both
 * are unused. Coordinates are read by polling the T_CS device. */
#define BOARD_TOUCH_XPT2046     1
#define BOARD_PIN_TOUCH_CS      3       /* XPT2046 chip-select (T_CS) */
#define BOARD_PIN_TOUCH_MOSI    11      /* shared with LCD_MOSI / radio MOSI */
#define BOARD_PIN_TOUCH_MISO    13      /* shared with LCD_MISO / radio MISO */
#define BOARD_PIN_TOUCH_CLK     12      /* shared with LCD_CLK / radio SCK */
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

/* ---- SubGHz / CC1101 (shared SPI bus with LCD + SD) ---- */
#define BOARD_PIN_CC1101_SCK    12      /* Shared with LCD_CLK and SD_SCLK */
#define BOARD_PIN_CC1101_CSN    15      /* FIXED: was 10 (clashed with LCD_CS) */
#define BOARD_PIN_CC1101_MISO   13      /* Shared with SD_MISO */
#define BOARD_PIN_CC1101_MOSI   11      /* Shared with LCD_MOSI and SD_MOSI */
#define BOARD_PIN_CC1101_GDO0   6       /* FIXED: was 9 (clashed with I2C SCL) */
#define BOARD_PIN_CC1101_GDO2   UINT16_MAX /* unused — GPIO4 is the SD slot CS */
#define BOARD_CC1101_SPI_SHARED 1

/* ---- NRF24L01 (optional, shared SPI bus) ---- */
#define BOARD_PIN_NRF24_SCK     BOARD_PIN_CC1101_SCK
#define BOARD_PIN_NRF24_MISO    BOARD_PIN_CC1101_MISO
#define BOARD_PIN_NRF24_MOSI    BOARD_PIN_CC1101_MOSI
#define BOARD_PIN_NRF24_CSN     14
#define BOARD_PIN_NRF24_CE      16
#define BOARD_HAS_NRF24         1

/* ---- IR ----
 * WIRING: TX = IR_OUT (LED driver, GPIO2), RX = IR_SIG (receiver out, GPIO1).
 * These were swapped; the schematic labels the header pins the other way.
 * Self-test: with a phone camera you should see GPIO2 blink when sending. */
#define BOARD_PIN_IR_TX         2
#define BOARD_PIN_IR_RX         1

/* ---- NFC / PN532 (reserved I2C 8/9, disabled in v1 — no module in wiring) ---- */
#define BOARD_PIN_NFC_SCL       9
#define BOARD_PIN_NFC_SDA       8
#define BOARD_PIN_NFC_IRQ       UINT16_MAX
#define BOARD_PIN_NFC_RST       UINT16_MAX
#define BOARD_NFC_I2C_PORT      I2C_NUM_0

/* ---- Status LED (single, active-high) ---- */
#define BOARD_PIN_STATUS_LED    40

/* ---- Unused audio/RGB (not in wiring) ---- */
#define BOARD_PIN_SPEAKER_BCLK  -1
#define BOARD_PIN_SPEAKER_WCLK  -1
#define BOARD_PIN_SPEAKER_DOUT  -1
#define BOARD_PIN_WS2812_DATA   UINT16_MAX
#define BOARD_WS2812_LED_COUNT  0
#define BOARD_PIN_MIC_DATA      UINT16_MAX
#define BOARD_PIN_MIC_CLK       UINT16_MAX

/* ---- Qwiic / External I2C (Grove, shared with NFC reservation) ---- */
#define BOARD_PIN_QWIIC_SDA     8
#define BOARD_PIN_QWIIC_SCL     9

/* ---- Features ---- */
#define BOARD_HAS_TOUCH         1   /* XPT2046 resistive (SPI) */
#define BOARD_HAS_ENCODER       0
#define BOARD_HAS_SD_CARD       1
/* The SD slot is on the LCD module and is optional: when no card is present
 * the desktop should stay usable instead of blocking on a "bad SD" prompt.
 * Set to 0 if you want the usual Flipper "insert SD card" behaviour. */
#define BOARD_SD_OPTIONAL       1
#define BOARD_HAS_BLE           1
#define BOARD_HAS_RGB_LED       0
#define BOARD_HAS_VIBRO         0
#define BOARD_HAS_SPEAKER       0
#define BOARD_HAS_IR            1
#define BOARD_HAS_IBUTTON       0
#define BOARD_HAS_RFID          0
#define BOARD_HAS_NFC           0   /* Set to 1 once PN532 is wired + tested */
#define BOARD_HAS_SUBGHZ        1
#define BOARD_HAS_MIC           0

/* Power service fallbacks (no fuel gauge/charger on DIY board) */
#define BQ27220_ADDR                        0x55
#define BQ25896_CHARGE_LIMIT                1280
#define FURI_HAL_POWER_VIRTUAL_CAPACITY_MAH (1300U)
