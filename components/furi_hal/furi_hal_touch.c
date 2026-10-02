/**
 * @file furi_hal_touch.c
 * Touch controller driver.
 *
 * Two families are supported, selected by the board header:
 *
 *  - Capacitive touch (I2C, legacy driver API):
 *      - CST816S  (default): e.g. Waveshare C6-LCD-1.9    @ 0x15, data reg 0x00
 *      - AXS5106L (#define BOARD_TOUCH_AXS5106L): e.g.
 *                           Waveshare C6-Touch-LCD-1.47  @ 0x63, data reg 0x01
 *
 *  - Resistive touch (SPI, shared bus with the LCD):
 *      - XPT2046  (#define BOARD_TOUCH_XPT2046): e.g. the generic ESP32-S3 +
 *                 2.8" ST7789 DIY build. Shares MOSI/MISO/CLK with the display
 *                 and has its own chip-select. No hardware gesture engine —
 *                 the input layer derives swipes from coordinate travel.
 *
 * The I2C path uses the legacy i2c driver API (these chips don't work with
 * i2c_master.h). The SPI path reuses the display's bus and serialises against
 * it with furi_hal_spi_bus_lock()/unlock().
 */

#include "furi_hal_touch.h"
#include "boards/board.h"

#include <furi.h>
#include <esp_log.h>
#include <esp_err.h>
#include <driver/gpio.h>

#if defined(BOARD_TOUCH_XPT2046)
#include <driver/spi_master.h>
#include "furi_hal_spi_bus.h"
#else
#include <driver/i2c.h>
#endif

#define TAG "FuriHalTouch"

/* State shared by both back-ends */
static bool touch_initialized = false;
static volatile FuriThreadId touch_notify_thread = NULL;

#if defined(BOARD_TOUCH_XPT2046)
/* ============================================================================
 *  XPT2046 resistive touch controller (SPI)
 *
 *  The chip shares the LCD's SPI bus (MOSI/MISO/CLK) and has a dedicated CS.
 *  Every transaction is wrapped in the shared SPI-bus mutex so it can't
 *  interleave with a display DMA flush.
 *
 *  Coordinate calibration is panel-specific, so the raw window and axis
 *  orientation come from the board header. The defaults below are only a
 *  backstop for boards that don't override them.
 * ==========================================================================*/

#ifndef BOARD_TOUCH_SPI_HOST
#define BOARD_TOUCH_SPI_HOST BOARD_LCD_SPI_HOST
#endif
#ifndef BOARD_TOUCH_SPI_FREQ_HZ
#define BOARD_TOUCH_SPI_FREQ_HZ (2 * 1000 * 1000) /* XPT2046 max ~2 MHz */
#endif
#ifndef BOARD_TOUCH_SWAP_XY
#define BOARD_TOUCH_SWAP_XY false
#endif
#ifndef BOARD_TOUCH_MIRROR_X
#define BOARD_TOUCH_MIRROR_X false
#endif
#ifndef BOARD_TOUCH_MIRROR_Y
#define BOARD_TOUCH_MIRROR_Y false
#endif
#ifndef BOARD_TOUCH_H_RES
#define BOARD_TOUCH_H_RES BOARD_LCD_H_RES
#endif
#ifndef BOARD_TOUCH_V_RES
#define BOARD_TOUCH_V_RES BOARD_LCD_V_RES
#endif
#ifndef BOARD_TOUCH_RAW_X_MIN
#define BOARD_TOUCH_RAW_X_MIN 200
#endif
#ifndef BOARD_TOUCH_RAW_X_MAX
#define BOARD_TOUCH_RAW_X_MAX 3900
#endif
#ifndef BOARD_TOUCH_RAW_Y_MIN
#define BOARD_TOUCH_RAW_Y_MIN 200
#endif
#ifndef BOARD_TOUCH_RAW_Y_MAX
#define BOARD_TOUCH_RAW_Y_MAX 3900
#endif
/* Max spread between two back-to-back samples still treated as "a finger".
 * A floating panel is noisy; a real press is stable, so this rejects idle
 * noise that happens to land inside the raw window. */
#ifndef BOARD_TOUCH_RAW_TOLERANCE
#define BOARD_TOUCH_RAW_TOLERANCE 400
#endif

/* Control bytes: start bit | channel | 12-bit | single-ended | power-down.
 * Channel 5 = X, channel 1 = Y on the standard XPT2046 wiring. */
#define XPT2046_CMD_READ_X 0xD0u
#define XPT2046_CMD_READ_Y 0x90u

static spi_device_handle_t xpt2046_dev = NULL;

/* Raw ADC values of the last sample that passed the press test. Used by
 * diagnostics and by the touch-calibration tooling (raw values are what the
 * pixel mapping is derived from). */
static uint16_t xpt2046_last_raw_x = 0;
static uint16_t xpt2046_last_raw_y = 0;

void furi_hal_touch_get_last_raw(uint16_t* raw_x, uint16_t* raw_y) {
    if(raw_x) *raw_x = xpt2046_last_raw_x;
    if(raw_y) *raw_y = xpt2046_last_raw_y;
}

/* One 24-clock transfer: 8-bit command + 16 clocks carrying the left-aligned
 * 12-bit result in the last two bytes. */
static uint16_t xpt2046_spi_transfer(uint8_t cmd) {
    uint8_t tx[3] = {cmd, 0x00, 0x00};
    uint8_t rx[3] = {0x00, 0x00, 0x00};

    spi_transaction_t transaction = {
        .length = 3 * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    if(spi_device_transmit(xpt2046_dev, &transaction) != ESP_OK) {
        return 0;
    }

    return (uint16_t)((((uint16_t)rx[1] << 8) | rx[2]) >> 3) & 0x0FFFu;
}

#ifndef BOARD_TOUCH_MEDIAN_SAMPLES
static uint32_t xpt2046_abs_diff(uint16_t a, uint16_t b) {
    return (a > b) ? (uint32_t)(a - b) : (uint32_t)(b - a);
}
#endif

#ifdef BOARD_TOUCH_MEDIAN_SAMPLES
/* Median sampling for noisy panels: collect N samples per axis, sort, average
 * the middle 5. Random conversion noise averages out; a steady finger reads
 * stable. The middle-5 spread must still fit the tolerance, otherwise the
 * whole set is rejected (idle bus / wildly unsteady contact). Boards without
 * BOARD_TOUCH_MEDIAN_SAMPLES keep the original 2-sample path untouched. */
static uint16_t xpt2046_read_axis_median(uint8_t cmd, uint32_t* spread) {
    uint16_t s[BOARD_TOUCH_MEDIAN_SAMPLES];
    for(int i = 0; i < BOARD_TOUCH_MEDIAN_SAMPLES; i++) {
        s[i] = xpt2046_spi_transfer(cmd);
    }
    for(int i = 1; i < BOARD_TOUCH_MEDIAN_SAMPLES; i++) {
        uint16_t k = s[i];
        int j = i - 1;
        while(j >= 0 && s[j] > k) {
            s[j + 1] = s[j];
            j--;
        }
        s[j + 1] = k;
    }
    int mid = BOARD_TOUCH_MEDIAN_SAMPLES / 2;
    int lo = mid - 2;
    int hi = mid + 2;
    if(lo < 0) lo = 0;
    if(hi >= BOARD_TOUCH_MEDIAN_SAMPLES) hi = BOARD_TOUCH_MEDIAN_SAMPLES - 1;
    uint32_t acc = 0;
    for(int i = lo; i <= hi; i++) acc += s[i];
    if(spread) *spread = (uint32_t)s[hi] - s[lo];
    return (uint16_t)(acc / (uint32_t)(hi - lo + 1));
}
#endif

/* Sample both axes; return 1 = press, 0 = released, -1 = bus busy.
 * wait=true blocks on the bus lock (original behaviour); wait=false takes it
 * only if free so a 4 ms input poll never stalls behind a display frame. */
static int xpt2046_read_raw_ex(uint16_t* raw_x, uint16_t* raw_y, bool wait) {
    if(!xpt2046_dev) return 0;

    if(wait) {
        furi_hal_spi_bus_lock();
    } else if(!furi_hal_spi_bus_try_lock()) {
        return -1;
    }
    /* First conversion after a channel switch can be stale, so take two. */
    xpt2046_spi_transfer(XPT2046_CMD_READ_X);
#ifdef BOARD_TOUCH_MEDIAN_SAMPLES
    uint32_t xspread = 0, yspread = 0;
    uint16_t x = xpt2046_read_axis_median(XPT2046_CMD_READ_X, &xspread);
    xpt2046_spi_transfer(XPT2046_CMD_READ_Y);
    uint16_t y = xpt2046_read_axis_median(XPT2046_CMD_READ_Y, &yspread);
    furi_hal_spi_bus_unlock();

    if(xspread > BOARD_TOUCH_RAW_TOLERANCE) return 0;
    if(yspread > BOARD_TOUCH_RAW_TOLERANCE) return 0;
#else
    uint16_t x1 = xpt2046_spi_transfer(XPT2046_CMD_READ_X);
    uint16_t x2 = xpt2046_spi_transfer(XPT2046_CMD_READ_X);
    xpt2046_spi_transfer(XPT2046_CMD_READ_Y);
    uint16_t y1 = xpt2046_spi_transfer(XPT2046_CMD_READ_Y);
    uint16_t y2 = xpt2046_spi_transfer(XPT2046_CMD_READ_Y);
    furi_hal_spi_bus_unlock();

    if(xpt2046_abs_diff(x1, x2) > BOARD_TOUCH_RAW_TOLERANCE) return 0;
    if(xpt2046_abs_diff(y1, y2) > BOARD_TOUCH_RAW_TOLERANCE) return 0;

    uint16_t x = (uint16_t)(((uint32_t)x1 + x2) / 2);
    uint16_t y = (uint16_t)(((uint32_t)y1 + y2) / 2);
#endif

    if(x < BOARD_TOUCH_RAW_X_MIN || x > BOARD_TOUCH_RAW_X_MAX) return 0;
    if(y < BOARD_TOUCH_RAW_Y_MIN || y > BOARD_TOUCH_RAW_Y_MAX) return 0;

    *raw_x = x;
    *raw_y = y;
    return 1;
}

/* Sample both axes; return true only when the samples look like a real press. */
static bool xpt2046_read_raw(uint16_t* raw_x, uint16_t* raw_y) {
    return xpt2046_read_raw_ex(raw_x, raw_y, true) > 0;
}

static uint16_t xpt2046_clamp(int32_t value, int32_t max) {
    if(value < 0) return 0;
    if(value > max) return (uint16_t)max;
    return (uint16_t)value;
}

/* Scale raw ADC values into panel pixel coordinates, then apply the board's
 * axis orientation.
 *
 * The panel's raw X/Y are aligned with the LCD's NATIVE portrait axes
 * (raw_x = short/240 axis, raw_y = long/320 axis). In landscape the screen
 * axes are transposed, so each raw axis must scale into the swapped target
 * dimension as well — otherwise screen coordinates get squashed into the
 * wrong range. */
static void xpt2046_map(uint16_t raw_x, uint16_t raw_y, uint16_t* screen_x, uint16_t* screen_y) {
    int32_t raw_dx = (int32_t)raw_x - BOARD_TOUCH_RAW_X_MIN;
    int32_t raw_dy = (int32_t)raw_y - BOARD_TOUCH_RAW_Y_MIN;

#if BOARD_TOUCH_SWAP_XY
    /* landscape: screen X <- panel long axis, screen Y <- panel short axis */
    int32_t x = raw_dy * (int32_t)(BOARD_TOUCH_H_RES - 1) /
                (int32_t)(BOARD_TOUCH_RAW_Y_MAX - BOARD_TOUCH_RAW_Y_MIN);
    int32_t y = raw_dx * (int32_t)(BOARD_TOUCH_V_RES - 1) /
                (int32_t)(BOARD_TOUCH_RAW_X_MAX - BOARD_TOUCH_RAW_X_MIN);
#else
    int32_t x = raw_dx * (int32_t)(BOARD_TOUCH_H_RES - 1) /
                (int32_t)(BOARD_TOUCH_RAW_X_MAX - BOARD_TOUCH_RAW_X_MIN);
    int32_t y = raw_dy * (int32_t)(BOARD_TOUCH_V_RES - 1) /
                (int32_t)(BOARD_TOUCH_RAW_Y_MAX - BOARD_TOUCH_RAW_Y_MIN);
#endif

#if BOARD_TOUCH_MIRROR_X
    x = (int32_t)(BOARD_TOUCH_H_RES - 1) - x;
#endif
#if BOARD_TOUCH_MIRROR_Y
    y = (int32_t)(BOARD_TOUCH_V_RES - 1) - y;
#endif

    *screen_x = xpt2046_clamp(x, (int32_t)(BOARD_TOUCH_H_RES - 1));
    *screen_y = xpt2046_clamp(y, (int32_t)(BOARD_TOUCH_V_RES - 1));
}

void furi_hal_touch_init(void) {
    ESP_LOGI(TAG, "Initializing XPT2046 (SPI) touch controller");

    if(xpt2046_dev) {
        touch_initialized = true;
        return;
    }

    furi_hal_spi_bus_init();

    spi_device_interface_config_t dev_config = {
        .clock_speed_hz = BOARD_TOUCH_SPI_FREQ_HZ,
        .mode = 0,
        .spics_io_num = BOARD_PIN_TOUCH_CS,
        .queue_size = 1,
    };

    esp_err_t err = spi_bus_add_device(BOARD_TOUCH_SPI_HOST, &dev_config, &xpt2046_dev);
    if(err != ESP_OK) {
        xpt2046_dev = NULL;
        ESP_LOGE(
            TAG,
            "XPT2046 spi_bus_add_device failed: %s (LCD bus not up yet?)",
            esp_err_to_name(err));
        /* Failsafe: a failed init would leave T_CS floating, and a floating
         * XPT2046 CS intermittently selects the chip, jamming the shared MISO
         * for the radio/SD on the same bus. Park it deselected instead. */
        gpio_config(&(gpio_config_t){
            .pin_bit_mask = 1ULL << BOARD_PIN_TOUCH_CS,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        });
        gpio_set_level(BOARD_PIN_TOUCH_CS, 1);
        return;
    }

    touch_initialized = true;
    ESP_LOGI(
        TAG,
        "XPT2046 ready (CS=%d, %d Hz, host %d)",
        BOARD_PIN_TOUCH_CS,
        BOARD_TOUCH_SPI_FREQ_HZ,
        BOARD_TOUCH_SPI_HOST);
}

bool furi_hal_touch_is_pressed(void) {
    if(!touch_initialized) return false;

    uint16_t raw_x = 0, raw_y = 0;
    return xpt2046_read_raw(&raw_x, &raw_y);
}

void furi_hal_touch_get_xy(uint16_t* x, uint16_t* y) {
    *x = 0;
    *y = 0;
    if(!touch_initialized) return;

    uint16_t raw_x = 0, raw_y = 0;
    if(!xpt2046_read_raw(&raw_x, &raw_y)) return;

    xpt2046_map(raw_x, raw_y, x, y);
}

bool furi_hal_touch_read(TouchData* data) {
    if(!data) return false;

    data->gesture = TouchGestureNone;
    data->finger_count = 0;
    data->x = 0;
    data->y = 0;

    if(!touch_initialized || !xpt2046_dev) return false;

    uint16_t raw_x = 0, raw_y = 0;
    if(!xpt2046_read_raw(&raw_x, &raw_y)) {
        /* Bus OK, just no finger on the panel. */
        return true;
    }

    xpt2046_last_raw_x = raw_x;
    xpt2046_last_raw_y = raw_y;
    xpt2046_map(raw_x, raw_y, &data->x, &data->y);
    data->finger_count = 1;
    return true;
}

int furi_hal_touch_try_read(TouchData* data) {
    if(!data) return 0;

    data->gesture = TouchGestureNone;
    data->finger_count = 0;
    data->x = 0;
    data->y = 0;

    if(!touch_initialized || !xpt2046_dev) return 0;

    uint16_t raw_x = 0, raw_y = 0;
    int rc = xpt2046_read_raw_ex(&raw_x, &raw_y, false);
    if(rc < 0) return -1; /* display owns the bus — skip this poll, keep state */

    if(rc > 0) {
        xpt2046_last_raw_x = raw_x;
        xpt2046_last_raw_y = raw_y;
        xpt2046_map(raw_x, raw_y, &data->x, &data->y);
        data->finger_count = 1;
    }
    return data->finger_count;
}

uint8_t furi_hal_touch_get_gesture(void) {
    /* No hardware gesture engine — the input layer derives swipes. */
    return 0;
}

bool furi_hal_touch_int_active(void) {
    /* IRQ is optional and left unconnected; polling is used instead. */
    return false;
}

void furi_hal_touch_set_notify_thread(void* thread_id) {
    touch_notify_thread = (FuriThreadId)thread_id;
}

#else /* !BOARD_TOUCH_XPT2046 — capacitive I2C touch controller */

/* Hardware configuration from board header */
#define TOUCH_I2C_ADDR     BOARD_TOUCH_I2C_ADDR
#define TOUCH_SCL_PIN      BOARD_PIN_TOUCH_SCL
#define TOUCH_SDA_PIN      BOARD_PIN_TOUCH_SDA
#define TOUCH_I2C_PORT     BOARD_TOUCH_I2C_PORT
#define TOUCH_I2C_FREQ_HZ  BOARD_TOUCH_I2C_FREQ_HZ
#define TOUCH_I2C_TIMEOUT  BOARD_TOUCH_I2C_TIMEOUT

/* ---- Controller-specific data layout --------------------------------------
 * raw[FINGER_IDX]            = finger count
 * raw[XY_OFFSET + 0/1]       = X high(&0x0F)/low byte
 * raw[XY_OFFSET + 2/3]       = Y high(&0x0F)/low byte
 * RST_LOW/HIGH_MS            = reset pulse timing for this chip
 */
#ifdef BOARD_TOUCH_AXS5106L
#define TOUCH_CHIP_NAME    "AXS5106L"
#define TOUCH_DATA_REG     0x01
#define TOUCH_DATA_LEN     14
#define TOUCH_FINGER_IDX   1
#define TOUCH_XY_OFFSET    2
#define TOUCH_RST_LOW_MS   200
#define TOUCH_RST_HIGH_MS  300
#else
#define TOUCH_CHIP_NAME    "CST816S"
#define TOUCH_DATA_REG     0x00
#define TOUCH_DATA_LEN     7
#define TOUCH_FINGER_IDX   2
#define TOUCH_XY_OFFSET    3
#define TOUCH_RST_LOW_MS   10
#define TOUCH_RST_HIGH_MS  50
#endif

static bool touch_i2c_read(uint8_t reg, uint8_t* data, size_t len) {
#ifdef BOARD_TOUCH_AXS5106L
    /* The AXS5106L does NOT support I2C repeated-start: it needs a STOP between
     * the register-address write and the read (write reg + STOP, then a fresh
     * read transaction). Using write_read_device (repeated-start) makes the
     * chip ACK the address probe but never return register data. */
    if(i2c_master_write_to_device(
           TOUCH_I2C_PORT, TOUCH_I2C_ADDR, &reg, 1, TOUCH_I2C_TIMEOUT) != ESP_OK) {
        return false;
    }
    return i2c_master_read_from_device(
               TOUCH_I2C_PORT, TOUCH_I2C_ADDR, data, len, TOUCH_I2C_TIMEOUT) == ESP_OK;
#else
    esp_err_t err = i2c_master_write_read_device(
        TOUCH_I2C_PORT, TOUCH_I2C_ADDR, &reg, 1, data, len, TOUCH_I2C_TIMEOUT);
    return (err == ESP_OK);
#endif
}

#ifndef BOARD_TOUCH_AXS5106L
static bool touch_i2c_write(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    esp_err_t err = i2c_master_write_to_device(
        TOUCH_I2C_PORT, TOUCH_I2C_ADDR, buf, 2, TOUCH_I2C_TIMEOUT);
    return (err == ESP_OK);
}
#endif

/* Read the controller's touch packet and decode finger count + first point. */
static bool touch_read_point(uint8_t* finger_count, uint16_t* x, uint16_t* y) {
    uint8_t raw[TOUCH_DATA_LEN] = {0};
    if(!touch_i2c_read(TOUCH_DATA_REG, raw, TOUCH_DATA_LEN)) {
        return false;
    }
    *finger_count = raw[TOUCH_FINGER_IDX];
    *x = ((uint16_t)(raw[TOUCH_XY_OFFSET + 0] & 0x0F) << 8) | raw[TOUCH_XY_OFFSET + 1];
    *y = ((uint16_t)(raw[TOUCH_XY_OFFSET + 2] & 0x0F) << 8) | raw[TOUCH_XY_OFFSET + 3];
    return true;
}

void furi_hal_touch_init(void) {
    ESP_LOGI(TAG, "Initializing %s touch controller", TOUCH_CHIP_NAME);

    /* Pulse the touch controller's reset line before talking to it. The
     * Waveshare C6-LCD boards hold the controller in reset unless the host
     * drives TP_RST high — without this the chip never ACKs on I2C. Active-low
     * reset: low for RST_LOW_MS, then high, then RST_HIGH_MS for the chip to
     * boot (the AXS5106L needs ~200/300ms, the CST816S much less). Boards with
     * no TP_RST pin (or a hardware pull-up, e.g. the 1.9) are unaffected. */
#ifdef BOARD_PIN_TOUCH_RST
    gpio_set_direction((gpio_num_t)BOARD_PIN_TOUCH_RST, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)BOARD_PIN_TOUCH_RST, 0);
    furi_delay_ms(TOUCH_RST_LOW_MS);
    gpio_set_level((gpio_num_t)BOARD_PIN_TOUCH_RST, 1);
    furi_delay_ms(TOUCH_RST_HIGH_MS);
#endif

    /* Initialize I2C bus (legacy driver, same as Waveshare demo) */
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = TOUCH_SDA_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = TOUCH_SCL_PIN,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = TOUCH_I2C_FREQ_HZ,
        .clk_flags = 0,
    };

    esp_err_t err = i2c_param_config(TOUCH_I2C_PORT, &conf);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(err));
        return;
    }

    err = i2c_driver_install(TOUCH_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(err));
        return;
    }

#ifdef BOARD_TOUCH_AXS5106L
    /* AXS5106L: there is no "normal mode" command like the CST816S; just verify
     * the chip answers by reading its device ID from register 0x08. */
    uint8_t id[2] = {0};
    if(touch_i2c_read(0x08, id, 2)) {
        ESP_LOGI(TAG, "AXS5106L device ID: 0x%02X%02X", id[0], id[1]);
    } else {
        ESP_LOGW(TAG, "AXS5106L not responding (addr 0x%02X)", TOUCH_I2C_ADDR);
    }
#else
    /* Switch to normal mode (same as Waveshare demo: write 0x00 to reg 0x00) */
    if(touch_i2c_write(0x00, 0x00)) {
        ESP_LOGI(TAG, "CST816S set to normal mode");
    } else {
        ESP_LOGW(TAG, "CST816S not responding (may wake on first touch)");
    }

    /* Disable auto-sleep so chip stays responsive to I2C polling */
    if(touch_i2c_write(0xFE, 0x01)) {
        ESP_LOGI(TAG, "CST816S auto-sleep disabled");
    } else {
        ESP_LOGW(TAG, "CST816S auto-sleep disable failed");
    }

    /* Try to read chip ID */
    uint8_t chip_id = 0;
    if(touch_i2c_read(0xA7, &chip_id, 1)) {
        ESP_LOGI(TAG, "CST816S chip ID: 0x%02X", chip_id);
    }
#endif

    touch_initialized = true;
    ESP_LOGI(TAG, "Touch init OK (I2C polling mode)");
}

bool furi_hal_touch_is_pressed(void) {
    if(!touch_initialized) return false;

    uint8_t finger_count = 0;
    uint16_t x = 0, y = 0;
    if(!touch_read_point(&finger_count, &x, &y)) {
        return false;
    }
    return (finger_count > 0);
}

void furi_hal_touch_get_xy(uint16_t* x, uint16_t* y) {
    *x = 0;
    *y = 0;
    if(!touch_initialized) return;

    uint8_t finger_count = 0;
    touch_read_point(&finger_count, x, y);
}

bool furi_hal_touch_read(TouchData* data) {
    if(!touch_initialized || !data) {
        if(data) {
            data->gesture = TouchGestureNone;
            data->finger_count = 0;
            data->x = 0;
            data->y = 0;
        }
        return false;
    }

    uint8_t raw[TOUCH_DATA_LEN] = {0};
    if(!touch_i2c_read(TOUCH_DATA_REG, raw, TOUCH_DATA_LEN)) {
        data->gesture = TouchGestureNone;
        data->finger_count = 0;
        data->x = 0;
        data->y = 0;
        return false;
    }

#ifdef BOARD_TOUCH_AXS5106L
    /* The AXS5106L doesn't report hardware gestures — leave gesture None and
     * let the input layer derive swipes from X/Y motion. */
    data->gesture = TouchGestureNone;
#else
    data->gesture = (TouchGesture)raw[1];
#endif
    data->finger_count = raw[TOUCH_FINGER_IDX];
    data->x = ((uint16_t)(raw[TOUCH_XY_OFFSET + 0] & 0x0F) << 8) | raw[TOUCH_XY_OFFSET + 1];
    data->y = ((uint16_t)(raw[TOUCH_XY_OFFSET + 2] & 0x0F) << 8) | raw[TOUCH_XY_OFFSET + 3];
    return true;
}

uint8_t furi_hal_touch_get_gesture(void) {
    if(!touch_initialized) return 0;

#ifdef BOARD_TOUCH_AXS5106L
    return 0; /* no hardware gestures on this controller */
#else
    uint8_t data[TOUCH_DATA_LEN] = {0};
    if(!touch_i2c_read(TOUCH_DATA_REG, data, TOUCH_DATA_LEN)) return 0;
    return data[1]; /* gesture ID */
#endif
}

bool furi_hal_touch_int_active(void) {
    /* No INT pin used — always return false, use polling instead */
    return false;
}

void furi_hal_touch_set_notify_thread(void* thread_id) {
    touch_notify_thread = (FuriThreadId)thread_id;
}

#endif /* BOARD_TOUCH_XPT2046 */
