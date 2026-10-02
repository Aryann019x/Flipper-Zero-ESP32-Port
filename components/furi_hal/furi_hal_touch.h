/**
 * @file furi_hal_touch.h
 * Touch HAL API. Back-end is chosen by the board header:
 *   - CST816S / AXS5106L capacitive touch over I2C
 *   - XPT2046 resistive touch over SPI (BOARD_TOUCH_XPT2046)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the board's touch controller (I2C or SPI).
 *  Safe to call even if touch hardware is absent — will log warning and return. */
void furi_hal_touch_init(void);

/** Check if a finger is currently touching the screen.
 *  @return true if finger detected */
bool furi_hal_touch_is_pressed(void);

/** Read touch coordinates in panel pixel space.
 *  @param[out] x  X coordinate (0..H_RES-1)
 *  @param[out] y  Y coordinate (0..V_RES-1) */
void furi_hal_touch_get_xy(uint16_t* x, uint16_t* y);

/** CST816S gesture IDs */
typedef enum {
    TouchGestureNone = 0x00,
    TouchGestureSlideUp = 0x01,
    TouchGestureSlideDown = 0x02,
    TouchGestureSlideLeft = 0x03,
    TouchGestureSlideRight = 0x04,
    TouchGestureSingleClick = 0x05,
    TouchGestureDoubleClick = 0x0B,
    TouchGestureLongPress = 0x0C,
} TouchGesture;

/** Combined touch data from a single I2C read */
typedef struct {
    TouchGesture gesture;
    uint8_t finger_count;
    uint16_t x;
    uint16_t y;
} TouchData;

/** Read all touch data in a single transaction.
 *  @param[out] data  Touch data struct to fill
 *  @return true if the read succeeded (finger_count is 0 when not touching) */
bool furi_hal_touch_read(TouchData* data);

/** Non-blocking variant for the input poll: never waits on the shared SPI
 *  bus, so a display frame in flight costs one skipped 4 ms poll instead of
 *  a stalled input thread (which drops short light taps).
 *  @param[out] data  Touch data struct to fill (untouched when busy)
 *  @return 1 if pressed, 0 if released, -1 if the bus was busy (skip poll) */
int furi_hal_touch_try_read(TouchData* data);

/** Raw controller values (not pixels) of the last sample that passed the
 *  press test. Zero until a press has been seen. Only meaningful on the
 *  XPT2046 path; used for calibration and diagnostics.
 *  @param[out] raw_x  Raw X ADC value (0..4095)
 *  @param[out] raw_y  Raw Y ADC value (0..4095) */
void furi_hal_touch_get_last_raw(uint16_t* raw_x, uint16_t* raw_y);

/** Read a hardware gesture ID where supported.
 *  @return gesture id (0=None, 1=SlideUp, 2=SlideDown, 3=SlideLeft, 4=SlideRight, 5=SingleClick).
 *          Always 0 on controllers without a gesture engine (XPT2046/AXS5106L). */
uint8_t furi_hal_touch_get_gesture(void);

/** Set the thread ID that will receive interrupt flags on touch events.
 *  @param thread_id  FuriThreadId to notify (flag bit 0 will be set) */
void furi_hal_touch_set_notify_thread(void* thread_id);

/** Check if the touch INT pin is active (low = touch event pending).
 *  Does NOT use I2C — safe to call when CST816S is sleeping.
 *  @return true if INT pin is low (touch active) */
bool furi_hal_touch_int_active(void);

#ifdef __cplusplus
}
#endif
