/**
 * @file furi_hal_display.c
 * Display HAL — board-driven configuration via boards/board.h
 *
 * Flipper GUI: 128x64 mono → aspect-fit scaled and centered on display
 */

#include "furi_hal_display.h"
#include "furi_hal_light.h"
#include "furi_hal_resources.h"
#include "furi_hal_spi_bus.h"
#include "boards/board.h"
#include <toolbox/saved_struct.h>

#include <string.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>
#if defined(BOARD_DISPLAY_ILI9341) && BOARD_DISPLAY_ILI9341
#include <esp_lcd_ili9341.h>
#endif
#include <freertos/semphr.h>

static const char* TAG = "FuriHalDisplay";

/* Display dimensions from board config */
#define LCD_H_RES BOARD_LCD_H_RES
#define LCD_V_RES BOARD_LCD_V_RES

/* Flipper framebuffer dimensions */
#define FB_WIDTH  128
#define FB_HEIGHT 64

/* Always-on left/right inset (in physical LCD pixels) kept as plain background
 * color so the UI never sits flush against the screen's left/right edges. A
 * board may override this in its board.h; otherwise this default applies
 * everywhere. Set to 0 to disable. */
#ifndef BOARD_LCD_SIDE_MARGIN
#define BOARD_LCD_SIDE_MARGIN 4
#endif
#define DISPLAY_SIDE_MARGIN BOARD_LCD_SIDE_MARGIN

/* Largest panel dimension. Sizes the stripe buffer, the scale LUTs and the SPI
 * transfer limit so that any rotation (portrait *or* landscape) fits. */
#define DISPLAY_MAX_DIM ((LCD_H_RES > LCD_V_RES) ? LCD_H_RES : LCD_V_RES)

/* Rotation the panel comes up with before the user has chosen one. Boards may
 * override it (e.g. a portrait module whose UI is nicer in landscape). */
#ifndef BOARD_LCD_DEFAULT_ROTATION
#define BOARD_LCD_DEFAULT_ROTATION 0
#endif

/* Live geometry for the current rotation. lcd_h_res/lcd_v_res are the panel
 * size as the UI sees it (swapped in landscape); scaled_w/scaled_h is the
 * aspect-fit size of the 128x64 mono framebuffer inside it, and margin_x/
 * margin_y the centering offsets. All of it is recomputed by
 * display_update_layout() whenever the orientation changes. */
static uint16_t lcd_h_res;
static uint16_t lcd_v_res;
static uint16_t scaled_w;
static uint16_t scaled_h;
static uint16_t margin_x;
static uint16_t margin_y;

/* Colors from board config — both are set at runtime so the user can pick
 * UI Background (fg_color, the field that fills "unset" mono pixels) and
 * UI Foreground (bg_color, what fills "set" pixels = drawn UI elements). */
static uint16_t fg_color;
static uint16_t bg_color;

/* SPI configuration from board config */
#define LCD_SPI_HOST   BOARD_LCD_SPI_HOST
#define LCD_SPI_FREQ   BOARD_LCD_SPI_FREQ_HZ
#define LCD_CMD_BITS   BOARD_LCD_CMD_BITS
#define LCD_PARAM_BITS BOARD_LCD_PARAM_BITS

/* Stripe-based rendering: render & DMA-send N lines at a time.
 * Reduces DMA buffer from full-frame (~100KB) to a small stripe (~5KB). */
#define STRIPE_HEIGHT 8

static esp_lcd_panel_handle_t panel_handle = NULL;
static bool panel_is_asleep = false; /* guards the sleep/wake pair */
static uint16_t* rgb565_buf = NULL; // STRIPE_HEIGHT lines x DISPLAY_MAX_DIM
static SemaphoreHandle_t lcd_flush_done = NULL;
static uint8_t x_scale_lut[DISPLAY_MAX_DIM];
static uint8_t y_scale_lut[DISPLAY_MAX_DIM];
/* 0..3 — the panel's own mounting as defined by the board header is 0; see
 * furi_hal_display_set_rotation(). */
static uint8_t display_rotation = 0;

static bool furi_hal_display_flush_done_callback(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t* edata,
    void* user_ctx) {
    UNUSED(panel_io);
    UNUSED(edata);
    UNUSED(user_ctx);

    BaseType_t yield = pdFALSE;
    if(lcd_flush_done) {
        xSemaphoreGiveFromISR(lcd_flush_done, &yield);
    }

    return yield == pdTRUE;
}

static void furi_hal_display_prepare_flush(void) {
    if(!lcd_flush_done) return;

    xSemaphoreTake(lcd_flush_done, 0);
}

static void furi_hal_display_wait_flush(void) {
    if(!lcd_flush_done) return;

    if(xSemaphoreTake(lcd_flush_done, pdMS_TO_TICKS(250)) != pdTRUE) {
        ESP_LOGW(TAG, "Timed out waiting for LCD flush");
    }
}

static void furi_hal_display_init_scale_lut(void) {
    for(size_t x = 0; x < scaled_w; x++) {
        x_scale_lut[x] = (x * FB_WIDTH) / scaled_w;
    }

    for(size_t y = 0; y < scaled_h; y++) {
        y_scale_lut[y] = (y * FB_HEIGHT) / scaled_h;
    }
}

/* ─────────────────────── Orientation (portrait / landscape) ─────────────────
 * The board header describes the module's own mounting; that is rotation 0.
 * The user can rotate the UI in 90° steps from Settings → Interface → Screen
 * Rotation, and the choice is persisted so the panel comes up in the requested
 * orientation instead of flipping once a service loads it.
 *
 * MADCTL for rotation N follows the TFT_eSPI ILI9341 table:
 *   0: MX / 1: MV / 2: MY / 3: MX|MY|MV  (+BGR, +swap for odd N)
 * so MX = base_MX ^ bit0 ^ bit1, MY = base_MY ^ bit1, MV = bit0. The old code
 * used MX = base_MX ^ bit0 which made 2/3 mirrored (3 showed as MY|MV). */
#define DISPLAY_SETTINGS_PATH    "/int/.display.settings"
#define DISPLAY_SETTINGS_MAGIC   0x44
/* v5: esp32s3_st7789_diy header fixed to portrait-native 240x320 dims with
 * default rotation 3 (was landscape 320x240 dims with default 0, which drew
 * 320-px rows into a 240-column panel = half the screen missing). Bumping
 * discards the stored 0 so the landscape default wins. Other boards fall
 * back to their own defaults (unchanged behaviour unless a custom rotation
 * was saved — it resets once). */
#define DISPLAY_SETTINGS_VERSION 0x05

typedef struct {
    uint8_t rotation;
} DisplaySettings;

static uint8_t display_rotation_load(void) {
    DisplaySettings st;
    const uint8_t fallback = (BOARD_LCD_DEFAULT_ROTATION > 3) ? 0 : BOARD_LCD_DEFAULT_ROTATION;

    if(!saved_struct_load(
           DISPLAY_SETTINGS_PATH,
           &st,
           sizeof(st),
           DISPLAY_SETTINGS_MAGIC,
           DISPLAY_SETTINGS_VERSION)) {
        return fallback;
    }

    return (st.rotation > 3) ? fallback : st.rotation;
}

static void display_rotation_save(uint8_t rotation) {
    DisplaySettings st = {.rotation = rotation};
    saved_struct_save(
        DISPLAY_SETTINGS_PATH, &st, sizeof(st), DISPLAY_SETTINGS_MAGIC, DISPLAY_SETTINGS_VERSION);
}

/* Mirror of display_update_layout's swap choice, without touching geometry. */
static bool display_rotation_is_swapped(void) {
    return (display_rotation & 1) != 0;
}

static void display_update_layout(void) {
    const bool swap = display_rotation_is_swapped();
    lcd_h_res = swap ? LCD_V_RES : LCD_H_RES;
    lcd_v_res = swap ? LCD_H_RES : LCD_V_RES;

    /* Scale the 128x64 framebuffer to the largest centered size that keeps its
     * aspect ratio, leaving the always-on side margin free on both sides. */
    uint16_t usable_w = lcd_h_res;
    if(usable_w > 2 * DISPLAY_SIDE_MARGIN) usable_w -= 2 * DISPLAY_SIDE_MARGIN;

    if((uint32_t)usable_w * FB_HEIGHT <= (uint32_t)lcd_v_res * FB_WIDTH) {
        scaled_w = usable_w;
        scaled_h = (usable_w * FB_HEIGHT) / FB_WIDTH;
    } else {
        scaled_h = lcd_v_res;
        scaled_w = (lcd_v_res * FB_WIDTH) / FB_HEIGHT;
    }

    if(scaled_w > DISPLAY_MAX_DIM) scaled_w = DISPLAY_MAX_DIM;
    if(scaled_h > DISPLAY_MAX_DIM) scaled_h = DISPLAY_MAX_DIM;

    margin_x = (lcd_h_res - scaled_w) / 2;
    margin_y = (lcd_v_res - scaled_h) / 2;

    furi_hal_display_init_scale_lut();
}

static void display_apply_rotation(void) {
    if(!panel_handle) return;

    const bool b0 = (display_rotation & 1) != 0;
    const bool b1 = (display_rotation & 2) != 0;
    const bool flip_x = ((bool)BOARD_LCD_MIRROR_X) ^ b0 ^ b1;
    const bool flip_y = ((bool)BOARD_LCD_MIRROR_Y) ^ b1;

    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, display_rotation_is_swapped()));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, flip_x, flip_y));
}

/* --- Boot splash ----------------------------------------------------
 * Painted right after the first clear, before the backlight comes on, so
 * the user sees a logo instead of a blank orange screen during the ~6s
 * service startup. Tiny built-in 5x7 glyphs; reuses the stripe buffer. */
static void display_paint_rect(size_t x, size_t y, size_t w, size_t h, uint16_t color);

static const uint8_t* splash_glyph(char c) {
    switch(c) {
    case 'F': { static const uint8_t g[] = {0x7F, 0x09, 0x09, 0x09, 0x01}; return g; }
    case 'l': { static const uint8_t g[] = {0x7F, 0x00, 0x00, 0x00, 0x00}; return g; }
    case 'i': { static const uint8_t g[] = {0x7D, 0x00, 0x00, 0x00, 0x00}; return g; }
    case 'p': { static const uint8_t g[] = {0x7F, 0x06, 0x06, 0x0F, 0x00}; return g; }
    case 'e': { static const uint8_t g[] = {0x1E, 0x17, 0x17, 0x17, 0x17}; return g; }
    case 'r': { static const uint8_t g[] = {0x1F, 0x01, 0x01, 0x02, 0x00}; return g; }
    case 'Z': { static const uint8_t g[] = {0x61, 0x51, 0x49, 0x45, 0x43}; return g; }
    case 'o': { static const uint8_t g[] = {0x3E, 0x61, 0x61, 0x61, 0x3E}; return g; }
    case 'E': { static const uint8_t g[] = {0x7F, 0x49, 0x49, 0x49, 0x41}; return g; }
    case 'S': { static const uint8_t g[] = {0x4E, 0x49, 0x49, 0x79, 0x39}; return g; }
    case 'P': { static const uint8_t g[] = {0x7F, 0x0F, 0x0F, 0x0F, 0x07}; return g; }
    case '3': { static const uint8_t g[] = {0x41, 0x49, 0x49, 0x7F, 0x41}; return g; }
    case '2': { static const uint8_t g[] = {0x61, 0x51, 0x49, 0x45, 0x43}; return g; }
    default:   return NULL;
    }
}

static void display_splash_text(uint16_t x, uint16_t y, const char* s, uint8_t scale, uint16_t color) {
    if(!rgb565_buf || !panel_handle) return;
    for(; *s; s++) {
        const uint8_t* g = splash_glyph(*s);
        if(g) {
            for(uint8_t cx = 0; cx < 5; cx++) {
                const uint8_t bits = g[cx];
                if(!bits) continue;
                for(uint8_t cy = 0; cy < 7; cy++) {
                    if(bits & (1u << cy)) {
                        display_paint_rect(x + cx * scale, y + cy * scale, scale, scale, color);
                    }
                }
            }
        }
        x += 6 * scale;
    }
}

static void display_splash_show(void) {
    /* Caller holds the SPI bus lock. Centered "Flipper / Zero / ESP32". */
    const uint16_t line1_w = 6 * 3 * 7 - 3;               /* "Flipper" @3x */
    const uint16_t line2_w = 6 * 3 * 4 - 3;               /* "Zero"    @3x */
    const uint16_t line3_w = 6 * 1 * 5 - 1;               /* "ESP32"   @1x */
    display_splash_text((lcd_h_res - line1_w) / 2, lcd_v_res * 30 / 100,
        "Flipper", 3, bg_color);
    display_splash_text((lcd_h_res - line2_w) / 2, lcd_v_res * 52 / 100,
        "Zero", 3, bg_color);
    display_splash_text((lcd_h_res - line3_w) / 2, lcd_v_res * 80 / 100,
        "ESP32", 1, bg_color);
}

/* Fill the whole panel with one color. Uses the stripe buffer in
 * STRIPE_HEIGHT-row bands: the previous one-line-per-transfer version needed a
 * blocking round-trip per row (hundreds of them), which the eye sees as the
 * boot screen slowly wiping in from the top. */
static void display_fill_color(uint16_t color) {
    if(!rgb565_buf || !panel_handle) return;

    const size_t band_px = (size_t)lcd_h_res * STRIPE_HEIGHT;
    for(size_t i = 0; i < band_px; i++) rgb565_buf[i] = color;

    furi_hal_spi_bus_lock();
    for(uint16_t y = 0; y < lcd_v_res; y += STRIPE_HEIGHT) {
        uint16_t chunk_h = lcd_v_res - y;
        if(chunk_h > STRIPE_HEIGHT) chunk_h = STRIPE_HEIGHT;

        furi_hal_display_prepare_flush();
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, lcd_h_res, y + chunk_h, rgb565_buf);
        furi_hal_display_wait_flush();
    }
    furi_hal_spi_bus_unlock();
}

/* Paint a solid-color rectangle using rgb565_buf as scratch. Caller must hold
 * the SPI bus lock. Chunks vertically at STRIPE_HEIGHT to stay within the
 * buffer's capacity (STRIPE_HEIGHT rows × SCALED_WIDTH cols). Used by the
 * commit path to keep the LCD margins in sync with fg_color changes. */
static void display_paint_rect(
    size_t x, size_t y, size_t w, size_t h, uint16_t color) {
    if(w == 0 || h == 0 || w > lcd_h_res) return;

    for(size_t y_off = 0; y_off < h; y_off += STRIPE_HEIGHT) {
        size_t chunk_h = h - y_off;
        if(chunk_h > STRIPE_HEIGHT) chunk_h = STRIPE_HEIGHT;

        const size_t px_count = w * chunk_h;
        for(size_t i = 0; i < px_count; i++) rgb565_buf[i] = color;

        furi_hal_display_prepare_flush();
        esp_lcd_panel_draw_bitmap(
            panel_handle, x, y + y_off, x + w, y + y_off + chunk_h, rgb565_buf);
        furi_hal_display_wait_flush();
    }
}

void furi_hal_display_init(void) {
    ESP_LOGI(TAG, "Initializing display for %s", BOARD_NAME);

    /* Keep the panel dark until it has been configured and cleared further
     * down. Otherwise the user watches the panel's power-on GRAM (junk) lit up
     * and then wiped away, which reads as a slow top-to-bottom boot scroll. */
    furi_hal_light_set(LightBacklight, 0);

    furi_hal_spi_bus_init();
    if(!lcd_flush_done) {
        lcd_flush_done = xSemaphoreCreateBinary();
        ESP_ERROR_CHECK(lcd_flush_done ? ESP_OK : ESP_ERR_NO_MEM);
    }

    /* Initialize SPI bus */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = gpio_lcd_din.pin,
        .miso_io_num = gpio_sdcard_miso.pin,
        .sclk_io_num = gpio_lcd_clk.pin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_MAX_DIM * STRIPE_HEIGHT * sizeof(uint16_t),
    };
    esp_err_t spi_err = spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    /* ESP_ERR_INVALID_STATE is OK — bus may already be initialized by SubGHz on shared-bus boards */
    if(spi_err != ESP_OK && spi_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(spi_err);
    }

    /* Create panel IO (SPI) */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = gpio_lcd_dc.pin,
        .cs_gpio_num = gpio_lcd_cs.pin,
        .pclk_hz = LCD_SPI_FREQ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 1,
        .on_color_trans_done = furi_hal_display_flush_done_callback,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_config, &io_handle));

    /* Create ST7789 / ILI9341 panel (selected by board header) */
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = gpio_lcd_rst.pin,
#if defined(BOARD_LCD_COLOR_ORDER_BGR) && BOARD_LCD_COLOR_ORDER_BGR
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel = 16,
    };
    /* --- Bring the ST7789 to a known-clean state, every boot --------------
     * A software reset (esp_restart) does NOT power-cycle the display: the
     * ST7789 keeps every register — MADCTL/colour-order, COLMOD, inversion,
     * rotation. That happens after an `esptool` flash *and* after any other
     * firmware that configured the panel differently ran before us. If any of
     * that lingers the R/B channels end up swapped — Flipper orange shows up as
     * blue — until the user pulls the battery (a real cold boot). So we do what
     * a thorough driver (TFT_eSPI) does on every init: hardware-reset pulse →
     * SWRESET command → full panel init → re-assert COLMOD. After that the panel
     * is in *our* configuration regardless of what ran before. */

    /* 1) Hardware reset pulse on RESX (HIGH → LOW → HIGH, generous timing). */
    gpio_config_t rst_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << gpio_lcd_rst.pin,
    };
    gpio_config(&rst_cfg);
    gpio_set_level((gpio_num_t)gpio_lcd_rst.pin, 1);   /* ensure a clean falling edge */
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)gpio_lcd_rst.pin, 0);   /* assert reset (active low) */
    vTaskDelay(pdMS_TO_TICKS(20));                      /* RESX min low is 10us; be generous */
    gpio_set_level((gpio_num_t)gpio_lcd_rst.pin, 1);   /* release reset */
    vTaskDelay(pdMS_TO_TICKS(150));                     /* ST7789: wait ≥120ms after reset */

    /* 2) Software reset (0x01) — re-loads all registers to factory defaults
     *    even if the RESX pulse above didn't fully take (e.g. a glitch on the
     *    line right after the ESP32 digital reset). The display still draws
     *    pixels in that case, so this command reaches it just fine. */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0x01 /* SWRESET */, NULL, 0));
    vTaskDelay(pdMS_TO_TICKS(150));

    ESP_ERROR_CHECK(
#if defined(BOARD_DISPLAY_ILI9341) && BOARD_DISPLAY_ILI9341
        esp_lcd_new_panel_ili9341(io_handle, &panel_config, &panel_handle)
#else
        esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle)
#endif
    );

    /* 3) esp_lcd does another RESX pulse, then SLPOUT + COLMOD + MADCTL. */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    /* Display corrections from board config: INVON/INVOFF and the GRAM gap.
     * The orientation bits (MADCTL swap/mirror) come from the rotation setting
     * instead — rotation 0 reproduces the board's own mount, and the user can
     * change it live (Settings → Interface → Screen Rotation). */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, BOARD_LCD_INVERT_COLOR));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, BOARD_LCD_GAP_X, BOARD_LCD_GAP_Y));
    display_rotation = display_rotation_load();
    display_apply_rotation();
    display_update_layout();

    /* 4) Belt-and-suspenders: pin down pixel format + normal display mode.
     *    COLMOD 0x55 = 16 bits/pixel (RGB565) — matches bits_per_pixel=16 above;
     *    NORON (0x13) = normal display mode (not partial/idle). Both are no-ops
     *    when the state is already right and cost nothing. */
    {
        const uint8_t colmod = 0x55;
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0x3A /* COLMOD */, &colmod, 1));
    }
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0x13 /* NORON */, NULL, 0));

    /* Turn on display */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    fg_color = BOARD_LCD_FG_COLOR;
    bg_color = BOARD_LCD_BG_COLOR;

    /* Allocate DMA-capable RGB565 stripe buffer (STRIPE_HEIGHT lines only).
     * Sized to the widest panel dimension so the same scratch buffer can paint
     * the full-width top/bottom margin strips in either orientation. */
    size_t stripe_bytes = DISPLAY_MAX_DIM * STRIPE_HEIGHT * sizeof(uint16_t);
    rgb565_buf = heap_caps_malloc(stripe_bytes, MALLOC_CAP_DMA);
    if(!rgb565_buf) {
        ESP_LOGE(TAG, "Failed to allocate RGB565 stripe buffer (%d bytes)",
                 (int)stripe_bytes);
        return;
    }

    /* Clear the screen to the UI field color, paint the boot splash while
     * the panel is still dark, and only then light the backlight — the user
     * sees the logo instead of a blank screen during service startup. The
     * notification service sets the real brightness level shortly after. */
    display_fill_color(fg_color);
    furi_hal_spi_bus_lock();
    display_splash_show();
    furi_hal_spi_bus_unlock();
    furi_hal_light_set(LightBacklight, 255);

    ESP_LOGI(TAG, "Display initialized (%dx%d, rotation=%d, scaled %dx%d, stripe=%d lines, buf=%d bytes)",
             FB_WIDTH, FB_HEIGHT, display_rotation, scaled_w, scaled_h, STRIPE_HEIGHT, (int)stripe_bytes);
}

void furi_hal_display_commit(const uint8_t* data, uint32_t size) {
    UNUSED(size);
    if(!panel_handle || !rgb565_buf || !data) return;

    /*
     * Stripe-based rendering: render STRIPE_HEIGHT lines into the small DMA
     * buffer, send via SPI, then render the next stripe.
     * Saves ~95KB of internal DMA-capable SRAM.
     *
     * u8g2 tile buffer layout:
     * 8 pages of 128 bytes each (128 columns × 8 pages = 1024 bytes)
     * Byte at [page * 128 + x]: bit n = pixel at (x, page*8 + n)
     * Bit 0 = top pixel in tile, bit 7 = bottom pixel in tile
     */
    /* Margins under one short lock hold (paint_rect does SPI transfers).
     * The stripe loop below locks per-stripe instead of per-frame: holding one
     * lock across the whole frame (render + ~30 DMAs) starves other bus users
     * — the 4ms input poll then misses short light taps (only long hard
     * presses survive). Touch/SD/radio transfers slip between stripes. */
    furi_hal_spi_bus_lock();

    /* Repaint the margin strips with the current fg_color every frame so the
     * entire screen tracks UI Color changes. Without this the init-time fill
     * persists and the margins keep showing the boot-time orange while the
     * central framebuffer area updates.
     *
     * Top/bottom strips span the full width; left/right strips (the always-on
     * DISPLAY_SIDE_MARGIN inset, plus any extra from aspect-fit centering) span
     * only the height of the scaled image so they don't overdraw the corners. */
    if(margin_y > 0) {
        display_paint_rect(0, 0, lcd_h_res, margin_y, fg_color);
        display_paint_rect(
            0, margin_y + scaled_h,
            lcd_h_res, lcd_v_res - margin_y - scaled_h, fg_color);
    }
    if(margin_x > 0) {
        display_paint_rect(0, margin_y, margin_x, scaled_h, fg_color);
        display_paint_rect(
            margin_x + scaled_w, margin_y,
            lcd_h_res - margin_x - scaled_w, scaled_h, fg_color);
    }

    furi_hal_spi_bus_unlock();

    for(size_t stripe_y = 0; stripe_y < scaled_h; stripe_y += STRIPE_HEIGHT) {
        size_t stripe_h = STRIPE_HEIGHT;
        if(stripe_y + stripe_h > scaled_h) {
            stripe_h = scaled_h - stripe_y;
        }

        /* Render stripe into buffer (CPU only — no lock needed) */
        for(size_t row = 0; row < stripe_h; row++) {
            const size_t sy = stripe_y + row;
            const uint8_t mono_y = y_scale_lut[sy];
            const size_t page = mono_y >> 3;
            const uint8_t bit_mask = 1U << (mono_y & 0x07);
            uint16_t* dst = &rgb565_buf[row * scaled_w];

            for(size_t sx = 0; sx < scaled_w; sx++) {
                const uint8_t mono_x = x_scale_lut[sx];
                const bool pixel_set = (data[page * FB_WIDTH + mono_x] & bit_mask) != 0;
                dst[sx] = pixel_set ? bg_color : fg_color;
            }
        }

        /* DMA send this stripe under a short lock hold */
        furi_hal_spi_bus_lock();
        furi_hal_display_prepare_flush();
        esp_lcd_panel_draw_bitmap(
            panel_handle,
            margin_x, margin_y + stripe_y,
            margin_x + scaled_w, margin_y + stripe_y + stripe_h,
            rgb565_buf);
        furi_hal_display_wait_flush();
        furi_hal_spi_bus_unlock();
    }
}

void furi_hal_display_set_backlight(uint8_t brightness) {
    /* brightness 0 means "screen off" here, so pair it with panel sleep; GRAM
     * is retained, so waking shows the last frame with no re-init. */
    if(brightness == 0) {
        furi_hal_light_set(LightBacklight, 0);
        furi_hal_display_sleep();
    } else {
        furi_hal_display_wakeup();
        furi_hal_light_set(LightBacklight, brightness);
    }
}

void furi_hal_display_sleep(void) {
    if(!panel_handle || panel_is_asleep) return;
    furi_hal_spi_bus_lock();
    /* DISPOFF blanks output; SLPIN also stops the oscillator/DC-DC. GRAM kept. */
    esp_lcd_panel_disp_on_off(panel_handle, false);
    esp_lcd_panel_disp_sleep(panel_handle, true);
    furi_hal_spi_bus_unlock();
    panel_is_asleep = true;
}

void furi_hal_display_wakeup(void) {
    if(!panel_handle || !panel_is_asleep) return;
    furi_hal_spi_bus_lock();
    /* SLPOUT (driver waits ~100 ms to settle) then DISPON. */
    esp_lcd_panel_disp_sleep(panel_handle, false);
    esp_lcd_panel_disp_on_off(panel_handle, true);
    furi_hal_spi_bus_unlock();
    panel_is_asleep = false;
}

bool furi_hal_display_is_asleep(void) {
    return panel_is_asleep;
}

uint16_t furi_hal_display_get_h_res(void) {
    return lcd_h_res;
}

uint16_t furi_hal_display_get_v_res(void) {
    return lcd_v_res;
}

uint8_t furi_hal_display_get_rotation(void) {
    return display_rotation;
}

void furi_hal_display_set_rotation(uint8_t rotation) {
    if(rotation > 3) rotation = 0;

    display_rotation = rotation;
    display_rotation_save(rotation);

    if(!panel_handle) return;

    furi_hal_spi_bus_lock();
    display_apply_rotation();
    display_update_layout();
    furi_hal_spi_bus_unlock();

    /* Repaint the field immediately: the panel keeps the old GRAM contents
     * until something redraws, and the stale margins would sit at the old
     * geometry until the next frame otherwise. */
    display_fill_color(fg_color);

    ESP_LOGI(TAG, "Display rotation set to %d (%ux%u)", display_rotation, lcd_h_res, lcd_v_res);
}

esp_lcd_panel_handle_t furi_hal_display_get_panel_handle(void) {
    return panel_handle;
}

void furi_hal_display_set_fg_color(uint16_t color) {
    fg_color = color;
}

uint16_t furi_hal_display_get_fg_color(void) {
    return fg_color;
}

void furi_hal_display_set_bg_color(uint16_t color) {
    bg_color = color;
}

uint16_t furi_hal_display_get_bg_color(void) {
    return bg_color;
}
