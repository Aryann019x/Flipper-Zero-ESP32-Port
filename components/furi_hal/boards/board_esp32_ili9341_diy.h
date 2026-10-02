/**
 * @file board_esp32_ili9341_diy.h
 * Board definition: Generic ESP-WROOM32 (ESP32 classic) + ILI9341 2.4" 240x320 + 5-way buttons
 *
 * MCU:      ESP32 (dual-core LX6, classic WROOM32, 4MB flash, NO PSRAM)
 * Display:  ILI9341 240x320 RGB565 via VSPI (23/19/18)
 * Input:    5-way tactile pad (active-low) + BOOT button as BACK
 * SD Card:  none in v1 wiring (SPI bus shared, CS free for future)
 * SubGHz:   CC1101 optional via shared VSPI (CS 15)
 * NRF24:    optional via shared VSPI (CS 15 / CE free, pick one radio at a time)
 * IR:       none in v1 wiring (add later on free pins if needed)
 * I2C:      Grove sensors on 21/22
 *
 * LIMITATIONS (read before opening a PR upstream):
 *  - Classic ESP32 has ~520KB SRAM, no PSRAM on WROOM32, 4MB flash.
 *    This firmware was designed for S3+8MB PSRAM+16MB flash. Expect OOM in
 *    WiFi/BLE/SubGHz heavy paths. Use a WROVER (PSRAM) if possible and
 *    disable heavy apps via fam_config.py (see sdkconfig.defaults file).
 *  - No native USB-OTG on classic ESP32. BadUSB-USB / qFlipper-USB /
 *    USB-Storage are stubbed (furi_hal_usb_hid_stub). Use UART0 + BLE only.
 *  - GPIO34/35 are INPUT-ONLY, no internal pull-up/down. They REQUIRE
 *    external 10k pull-ups to 3V3. The input driver below disables the
 *    internal pull-up for those two pins (see target_input.c).
 *  - 5-way pad has no BACK key. BOOT/GPIO0 is mapped to BACK. Long-press OK
 *    also emits BACK as fallback (see target_input.c).
 */

#pragma once

/* ---- Board metadata ---- */
#define BOARD_NAME        "ESP32 ILI9341 DIY"
#define BOARD_ID          "esp32_ili9341_diy"
#define BOARD_TARGET      "esp32"

/* ---- 5-way + BOOT buttons (all active-low to GND) ---- */
#define BOARD_PIN_BTN_UP        34  /* input-only, ext pull-up required */
#define BOARD_PIN_BTN_DOWN      26
#define BOARD_PIN_BTN_LEFT      33  /* note: 33 is strapping-adjacent, OK as input */
#define BOARD_PIN_BTN_RIGHT     27
#define BOARD_PIN_BTN_OK        35  /* input-only, ext pull-up required */
#define BOARD_PIN_BTN_BACK      0   /* BOOT button as BACK */
#define BOARD_PIN_BUTTON_BOOT   0
#define BOARD_PIN_BATTERY_ADC   UINT16_MAX

/* ---- LCD Pins (ILI9341 via VSPI pins, driven as SPI2_HOST) ----
 * NOTE: VSPI pins 23/19/18 are routed through SPI2_HOST via the GPIO matrix
 * so LCD + CC1101 + NRF24 share one bus (furi_hal_spi.c / furi_hal_sd.c
 * hardcode SPI2_HOST for the shared bus). Do NOT change to SPI3_HOST. */
#define BOARD_PIN_LCD_MOSI      23
#define BOARD_PIN_LCD_MISO      19  /* shared with radio MISO + SD future */
#define BOARD_PIN_LCD_SCLK      18
#define BOARD_PIN_LCD_DC        16
#define BOARD_PIN_LCD_CS        17
#define BOARD_PIN_LCD_RST       5
#define BOARD_PIN_LCD_BL        32  /* Backlight PWM, active-high */

/* ---- LCD Display Configuration (portrait 240x320) ---- */
#define BOARD_DISPLAY_ILI9341   1   /* switches furi_hal_display.c to ili9341 panel */
#define BOARD_LCD_H_RES         240
#define BOARD_LCD_V_RES         320
#define BOARD_LCD_SPI_HOST      SPI2_HOST
#define BOARD_LCD_SPI_FREQ_HZ   (20 * 1000 * 1000)  /* 20MHz conservative on long Dupont wires */
#define BOARD_LCD_CMD_BITS      8
#define BOARD_LCD_PARAM_BITS    8
#define BOARD_LCD_SWAP_XY       false
/* MX must be set on these 2.4" ILI9341 modules: with it clear the whole panel
 * renders as a mirror image (glyphs flipped, lines reversed) because the
 * controller's power-on MADCTL is already mirrored relative to the module's
 * mounting. 0x48 = MX|BGR is the canonical portrait value (TFT_eSPI rot 0). */
#define BOARD_LCD_MIRROR_X      true
#define BOARD_LCD_MIRROR_Y      false
#define BOARD_LCD_INVERT_COLOR  false   /* ILI9341 2.4" typically non-inverted */
/* The panel is portrait-native, but the Flipper UI is a wide 2:1 layout, so it
 * reads far better turned 90°. Landscape = 1 or 3 (180° apart). On this
 * module rotation 1 renders upside-down (see device photo), so the default
 * is 3 = upright. Settings → Sound & Display → Screen Rotation changes it
 * live (a saved value wins). */
#define BOARD_LCD_DEFAULT_ROTATION 3
#define BOARD_LCD_GAP_X         0
#define BOARD_LCD_GAP_Y         0
#define BOARD_LCD_BL_ACTIVE_LOW false
#define BOARD_LCD_COLOR_ORDER_BGR true  /* most 2.4" ILI9341 modules are BGR */

/* Flipper framebuffer -> display color mapping (RGB565) */
#define BOARD_LCD_FG_COLOR      0xA0FD  /* Flipper Orange */
#define BOARD_LCD_BG_COLOR      0x0000  /* Black */

/* ---- SD Card (ILI9341 TFT SD slot, SD_CS on GPIO12 per wiring) ----
 * The TFT SD slot shares the VSPI bus (MOSI 23 / MISO 19 / SCK 18); its CS
 * is wired to GPIO12. Without a card the init just fails softly
 * (BOARD_SD_OPTIONAL=1 keeps the UI quiet about it).
 * NOTE: GPIO12 is a strapping pin (flash voltage). If the board boot-loops
 * with an SD card INSERTED at power-on, power up without the card (or move
 * SD CS to a free GPIO like 25 and update this define). */
#define BOARD_PIN_SD_CS         12
#define BOARD_PIN_SD_MISO       19

/* ---- Touch — NOT PRESENT (5-way buttons) ---- */
#define BOARD_PIN_TOUCH_SCL     UINT16_MAX
#define BOARD_PIN_TOUCH_SDA     UINT16_MAX
#define BOARD_PIN_TOUCH_RST     UINT16_MAX
#define BOARD_PIN_TOUCH_INT     UINT16_MAX
#define BOARD_TOUCH_I2C_ADDR    0x00
#define BOARD_TOUCH_I2C_PORT    I2C_NUM_0
#define BOARD_TOUCH_I2C_FREQ_HZ 0
#define BOARD_TOUCH_I2C_TIMEOUT 0

/* ---- SubGHz / CC1101 (shared VSPI with LCD) ---- */
#define BOARD_PIN_CC1101_SCK    18      /* Shared SCK */
#define BOARD_PIN_CC1101_CSN    15      /* Own CS, isolated from LCD_CS=17 */
#define BOARD_PIN_CC1101_MISO   19      /* Shared MISO */
#define BOARD_PIN_CC1101_MOSI   23      /* Shared MOSI */
#define BOARD_PIN_CC1101_GDO0   4       /* Free GPIO, interrupt-capable */
#define BOARD_PIN_CC1101_GDO2   13      /* Free GPIO, interrupt-capable */
#define BOARD_CC1101_SPI_SHARED 1

/* ---- NRF24L01 (optional, same VSPI — use EITHER CC1101 OR NRF24, not both) ---- */
#define BOARD_PIN_NRF24_SCK     BOARD_PIN_CC1101_SCK
#define BOARD_PIN_NRF24_MISO    BOARD_PIN_CC1101_MISO
#define BOARD_PIN_NRF24_MOSI    BOARD_PIN_CC1101_MOSI
#define BOARD_PIN_NRF24_CSN     15      /* shares radio CS footprint */
#define BOARD_PIN_NRF24_CE      4       /* shares GDO0 footprint, output */
#define BOARD_HAS_NRF24         1

/* ---- IR (not in v1 wiring) ---- */
#define BOARD_PIN_IR_TX         UINT16_MAX
#define BOARD_PIN_IR_RX         UINT16_MAX

/* ---- NFC / PN532 (not in v1 wiring, Grove I2C reserved) ---- */
#define BOARD_PIN_NFC_SCL       22
#define BOARD_PIN_NFC_SDA       21
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

/* ---- Qwiic / Grove I2C ---- */
#define BOARD_PIN_QWIIC_SDA     21
#define BOARD_PIN_QWIIC_SCL     22

/* ---- Features (conservative for 4MB/no-PSRAM) ---- */
#define BOARD_HAS_TOUCH         0
#define BOARD_HAS_ENCODER       0
#define BOARD_HAS_SD_CARD       1   /* module SD slot, CS on GPIO25 (jumper) */
/* v1 has no SD card at all, so don't block the desktop on a "bad SD" prompt. */
#define BOARD_SD_OPTIONAL       1
/* BLE off: classic ESP32 has no PSRAM — the BT controller + Bluedroid eat
 * ~100KB of the ~320KB DRAM, which is exactly what pushed Sub-GHz/WiFi over
 * the edge (StoreProhibited on launch). The BLE serial console is the
 * tradeoff; WiFi still works. Flip back to 1 if you need BLE. */
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
