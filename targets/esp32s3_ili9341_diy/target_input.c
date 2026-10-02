/**
 * @file target_input.c
 * Input driver for esp32s3_ili9341_diy: XPT2046 resistive touch + BOOT button.
 *
 * The XPT2046 has no hardware gesture engine (unlike the CST816S capacitive
 * chip) and resistive swipes are unreliable, so the panel is driven as a
 * positional d-pad PLUS a direct-touch bottom button bar:
 *
 * Touch mapping (simple cross — tap where you want to go):
 *   upper-middle area  -> InputKeyUp
 *   lower-middle area  -> InputKeyDown
 *   middle-left area   -> InputKeyLeft
 *   middle-right area  -> InputKeyRight
 *   middle area        -> InputKeyOk      (tap)
 *   hold middle 650ms  -> InputKeyBack
 *   hold a direction   -> that direction repeats every 200ms (list scrolling)
 *
 * A quick touch reports its direction on release (one step per touch). Keeping
 * the finger down sends the direction after 220ms and then repeats it. The
 * middle rectangle (35% of each half-axis around the centre) is the click/Back
 * zone: tap = OK, hold = Back.
 *
 * BOOT button mapping (GPIO0):
 *   single click -> InputKeyOk
 *   double click -> InputKeyBack
 */

#include "target_input.h"

#include <furi_hal_touch.h>
#include <furi_hal_resources.h>
#include <boards/board.h>
#include <driver/gpio.h>
#include <esp_err.h>

#define TAG "InputTouchS3"

/* Polling/timing constants */
#define INPUT_BUTTON_DEBOUNCE_POLLS     2U
#define INPUT_BUTTON_SHORT_PRESS_MAX_MS 300U
#define INPUT_BUTTON_DOUBLE_CLICK_MS    250U

/* Positional zones: everything inside this percentage of each half-axis around
 * the screen centre is the middle "click" rectangle, everything outside it is
 * a direction area (dominant axis wins on the diagonal). No swipes involved. */
#define INPUT_TOUCH_DEAD_ZONE_PERCENT 35

/* Keep a direction area pressed this long -> send it, then start repeating. */
#define INPUT_TOUCH_DIR_HOLD_MS 220U

/* Repeat period while a direction area stays pressed. */
#define INPUT_TOUCH_REPEAT_MS 200U

/* Hold the middle area this long to mean "Back". */
#define INPUT_TOUCH_BACK_MS 650U

/* Resistive panels glitch (bus contention with the SD/LCD, released finger,
 * noise). A press only counts after it has been seen down for this many
 * consecutive polls (input polls every 4ms) and each poll stayed within
 * TOUCH_STABLE_PX of the first one. Without this, a single garbage read at
 * boot fired a phantom "Right" that opened an app by itself. */
#define INPUT_TOUCH_PRESS_POLLS 3U
#define INPUT_TOUCH_STABLE_PX   60U
/* Release must read "not pressed" this many polls in a row before the tap
 * counts as over, and a new tap within the lockout after a release is
 * ignored (release bounce). */
#define INPUT_TOUCH_RELEASE_POLLS  3U
#define INPUT_TOUCH_TAP_LOCKOUT_MS 150U

/* Set to 1 to printf raw touch coordinates / detected gestures (calibration). */
#define INPUT_TOUCH_DEBUG 0

/* ---- Shared event helpers ---- */

static void input_publish(FuriPubSub* pubsub, InputKey key, InputType type, uint32_t sequence) {
    InputEvent event = {
        .sequence_source = INPUT_SEQUENCE_SOURCE_TOUCH,
        .sequence_counter = sequence,
        .key = key,
        .type = type,
    };
    furi_pubsub_publish(pubsub, &event);
}

static void input_emit_short(FuriPubSub* pubsub, InputKey key, uint32_t sequence) {
    input_publish(pubsub, key, InputTypePress, sequence);
    input_publish(pubsub, key, InputTypeShort, sequence);
    input_publish(pubsub, key, InputTypeRelease, sequence);
}

/* ---- Touch state ---- */

typedef enum {
    TouchZoneNone = 0,
    TouchZoneUp,
    TouchZoneDown,
    TouchZoneLeft,
    TouchZoneRight,
    TouchZoneCenter, /* click / hold-to-Back */
} TouchZone;

static bool touch_interaction_active;
static TouchZone touch_zone;
static bool touch_dir_fired;
static bool touch_back_fired;
static uint32_t touch_started_at;
static uint32_t touch_repeated_at;

/* press-debounce state */
static uint8_t touch_press_polls;
static uint16_t touch_candidate_x;
static uint16_t touch_candidate_y;
/* release debounce + anti-double-tap: a resistive panel flickers on
 * press/release, and without these one physical tap fires 2-3 keys
 * (e.g. tap an app and it opens plus immediately does something). */
static uint8_t touch_release_polls;
static uint32_t touch_last_up_at;

static InputKey input_touch_zone_key(TouchZone zone) {
    switch(zone) {
    case TouchZoneUp:
        return InputKeyUp;
    case TouchZoneDown:
        return InputKeyDown;
    case TouchZoneLeft:
        return InputKeyLeft;
    case TouchZoneRight:
        return InputKeyRight;
    default:
        return InputKeyOk;
    }
}

/* Which key a touch position means. Coordinates are already in screen space
 * (0..H_RES-1 x 0..V_RES-1) thanks to furi_hal_touch's mapping. */
static TouchZone input_touch_zone_from_xy(uint16_t x, uint16_t y) {
    int32_t half_w = (int32_t)BOARD_TOUCH_H_RES / 2;
    int32_t half_h = (int32_t)BOARD_TOUCH_V_RES / 2;
    if(half_w < 1) half_w = 1;
    if(half_h < 1) half_h = 1;

    /* -100..100 = percent of the half-axis, so the middle rectangle does not
     * depend on the panel's aspect ratio. */
    int32_t nx = (((int32_t)x - half_w) * 100) / half_w;
    int32_t ny = (((int32_t)y - half_h) * 100) / half_h;
    int32_t anx = (nx < 0) ? -nx : nx;
    int32_t any = (ny < 0) ? -ny : ny;

    if(anx < INPUT_TOUCH_DEAD_ZONE_PERCENT && any < INPUT_TOUCH_DEAD_ZONE_PERCENT) {
        return TouchZoneCenter;
    }

    if(any >= anx) {
        return (ny < 0) ? TouchZoneUp : TouchZoneDown;
    }
    return (nx < 0) ? TouchZoneLeft : TouchZoneRight;
}

#if INPUT_TOUCH_DEBUG
static const char* input_touch_zone_name(TouchZone zone) {
    switch(zone) {
    case TouchZoneUp:
        return "up";
    case TouchZoneDown:
        return "down";
    case TouchZoneLeft:
        return "left";
    case TouchZoneRight:
        return "right";
    case TouchZoneCenter:
        return "center";
    default:
        return "none";
    }
}
#endif

static void input_touch_poll(FuriPubSub* pubsub, uint32_t* sequence_counter) {
    TouchData touch;
    if(!furi_hal_touch_read(&touch)) {
        return;
    }

    bool touching = (touch.finger_count > 0);
    uint32_t now = furi_get_tick();

    if(touching) {
        touch_release_polls = 0;
        if(!touch_interaction_active) {
            /* Debounce: the panel must report a finger down, at roughly the
             * same spot, for a few polls in a row before we act on it. */
            uint16_t jitter_x = (touch.x > touch_candidate_x) ? touch.x - touch_candidate_x
                                                              : touch_candidate_x - touch.x;
            uint16_t jitter_y = (touch.y > touch_candidate_y) ? touch.y - touch_candidate_y
                                                              : touch_candidate_y - touch.y;
            if(touch_press_polls == 0 || jitter_x > INPUT_TOUCH_STABLE_PX ||
               jitter_y > INPUT_TOUCH_STABLE_PX) {
                touch_candidate_x = touch.x;
                touch_candidate_y = touch.y;
                touch_press_polls = 1;
                return;
            }
            if(++touch_press_polls < INPUT_TOUCH_PRESS_POLLS) {
                return;
            }
            /* Anti-double-tap: a fresh press right after a release is the
             * old tap's bounce, not a new tap. */
            if((now - touch_last_up_at) < furi_ms_to_ticks(INPUT_TOUCH_TAP_LOCKOUT_MS)) {
                return;
            }

            /* touch-down accepted: remember which zone the finger landed in */
            touch_interaction_active = true;
            touch_dir_fired = false;
            touch_back_fired = false;
            touch_started_at = now;
            touch_repeated_at = now;
            touch_zone = input_touch_zone_from_xy(touch.x, touch.y);
#if INPUT_TOUCH_DEBUG
            /* Raw ADC values next to the mapped pixels: this is what the touch
             * calibration (BOARD_TOUCH_RAW_*_MIN/MAX) is derived from. */
            uint16_t raw_x = 0;
            uint16_t raw_y = 0;
            furi_hal_touch_get_last_raw(&raw_x, &raw_y);
            printf(
                "[TOUCH] down raw=%u,%u xy=%u,%u zone=%s\n",
                raw_x,
                raw_y,
                touch.x,
                touch.y,
                input_touch_zone_name(touch_zone));
#endif
        }

        uint32_t held = now - touch_started_at;

        if(touch_zone == TouchZoneCenter) {
            /* Middle area: hold it down to go Back (tap is sent on release). */
            if(!touch_back_fired && held >= furi_ms_to_ticks(INPUT_TOUCH_BACK_MS)) {
                input_emit_short(pubsub, InputKeyBack, ++(*sequence_counter));
                touch_back_fired = true;
#if INPUT_TOUCH_DEBUG
                printf("[TOUCH] hold center -> back\n");
#endif
            }
        } else if(touch_zone != TouchZoneNone) {
            InputKey dir = input_touch_zone_key(touch_zone);
            if(!touch_dir_fired) {
                /* Long enough in one direction area: send it and start
                 * repeating so lists scroll while the finger stays down. */
                if(held >= furi_ms_to_ticks(INPUT_TOUCH_DIR_HOLD_MS)) {
                    input_emit_short(pubsub, dir, ++(*sequence_counter));
                    touch_dir_fired = true;
                    touch_repeated_at = now;
#if INPUT_TOUCH_DEBUG
                    printf("[TOUCH] hold %s\n", input_touch_zone_name(touch_zone));
#endif
                }
            } else if((now - touch_repeated_at) >= furi_ms_to_ticks(INPUT_TOUCH_REPEAT_MS)) {
                input_emit_short(pubsub, dir, ++(*sequence_counter));
                touch_repeated_at = now;
            }
        }
    } else {
        touch_press_polls = 0;

        if(!touch_interaction_active) {
            return;
        }

        /* Release debounce: the panel flickers on lift-off, so only count
         * the tap as over after a few clean "not pressed" polls in a row. */
        if(++touch_release_polls < INPUT_TOUCH_RELEASE_POLLS) {
            return;
        }
        touch_release_polls = 0;

        /* touch-up: a quick touch reports its zone once */
        touch_interaction_active = false;
        touch_last_up_at = now;

        if(touch_zone == TouchZoneCenter) {
            if(!touch_back_fired) {
                input_emit_short(pubsub, InputKeyOk, ++(*sequence_counter));
#if INPUT_TOUCH_DEBUG
                printf("[TOUCH] tap center -> ok\n");
#endif
            }
        } else if(touch_zone != TouchZoneNone && !touch_dir_fired) {
            input_emit_short(pubsub, input_touch_zone_key(touch_zone), ++(*sequence_counter));
#if INPUT_TOUCH_DEBUG
            printf("[TOUCH] tap %s\n", input_touch_zone_name(touch_zone));
#endif
        }

        touch_zone = TouchZoneNone;
    }
}

/* ---- BOOT button (single click = OK, double click = Back) ---- */

#if !BOARD_HAS_BUTTONS_PAD

typedef struct {
    bool raw_pressed;
    bool debounced_pressed;
    uint8_t debounce_polls;
    uint32_t press_started_at;
    bool pending_single_click;
    bool second_click_started;
    uint32_t first_click_released_at;
} InputBootButtonState;

static InputBootButtonState boot_button;

static uint32_t input_elapsed_ticks(uint32_t started_at, uint32_t now) {
    return now - started_at;
}

static bool input_boot_button_is_pressed(void) {
    return gpio_get_level((gpio_num_t)gpio_button_boot.pin) == 0;
}

static void input_boot_button_init(void) {
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << gpio_button_boot.pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "BOOT button gpio_config failed: %s", esp_err_to_name(err));
    }
}

static void input_boot_button_init_state(InputBootButtonState* state) {
    state->raw_pressed = input_boot_button_is_pressed();
    state->debounced_pressed = state->raw_pressed;
    state->debounce_polls = INPUT_BUTTON_DEBOUNCE_POLLS;
    state->press_started_at = 0;
    state->pending_single_click = false;
    state->second_click_started = false;
    state->first_click_released_at = 0;
}

static void input_boot_button_reset_clicks(InputBootButtonState* state) {
    state->pending_single_click = false;
    state->second_click_started = false;
    state->first_click_released_at = 0;
}

static void input_boot_button_handle_press(InputBootButtonState* state, uint32_t now) {
    state->press_started_at = now;
    if(state->pending_single_click) {
        state->second_click_started = true;
    }
}

static void input_boot_button_handle_release(
    InputBootButtonState* state,
    FuriPubSub* pubsub,
    uint32_t now,
    uint32_t short_press_max_ticks,
    uint32_t double_click_ticks,
    uint32_t* sequence_counter) {
    bool is_short_press = input_elapsed_ticks(state->press_started_at, now) <= short_press_max_ticks;

    if(!is_short_press) {
        /* Long press: no action, but finish a pending click pair if any. */
        if(state->pending_single_click && state->second_click_started) {
            input_emit_short(pubsub, InputKeyOk, ++(*sequence_counter));
        }
        input_boot_button_reset_clicks(state);
        return;
    }

    if(!state->pending_single_click) {
        /* First click — wait to see whether a second follows. */
        state->pending_single_click = true;
        state->second_click_started = false;
        state->first_click_released_at = now;
        return;
    }

    if(input_elapsed_ticks(state->first_click_released_at, now) <= double_click_ticks) {
        input_emit_short(pubsub, InputKeyBack, ++(*sequence_counter));
        input_boot_button_reset_clicks(state);
        return;
    }

    input_emit_short(pubsub, InputKeyOk, ++(*sequence_counter));
    state->pending_single_click = true;
    state->second_click_started = false;
    state->first_click_released_at = now;
}

static void input_boot_button_poll(
    InputBootButtonState* state,
    FuriPubSub* pubsub,
    uint32_t now,
    uint32_t short_press_max_ticks,
    uint32_t double_click_ticks,
    uint32_t* sequence_counter) {
    bool raw_pressed = input_boot_button_is_pressed();

    if(raw_pressed == state->raw_pressed) {
        if(state->debounce_polls < INPUT_BUTTON_DEBOUNCE_POLLS) {
            state->debounce_polls++;
        }
    } else {
        state->raw_pressed = raw_pressed;
        state->debounce_polls = 1;
    }

    if((state->debounce_polls >= INPUT_BUTTON_DEBOUNCE_POLLS) &&
       (state->debounced_pressed != state->raw_pressed)) {
        state->debounced_pressed = state->raw_pressed;
        if(state->debounced_pressed) {
            input_boot_button_handle_press(state, now);
        } else {
            input_boot_button_handle_release(
                state, pubsub, now, short_press_max_ticks, double_click_ticks, sequence_counter);
        }
    }

    if(state->pending_single_click && !state->second_click_started &&
       (input_elapsed_ticks(state->first_click_released_at, now) > double_click_ticks)) {
        input_emit_short(pubsub, InputKeyOk, ++(*sequence_counter));
        input_boot_button_reset_clicks(state);
    }
}

#endif /* !BOARD_HAS_BUTTONS_PAD */

/* ---- Optional 6-button pad ---- */

#if BOARD_HAS_BUTTONS_PAD

#define PAD_BUTTON_COUNT       6
#define PAD_DEBOUNCE_POLLS     2U
#define PAD_LONG_PRESS_MS      500U
#define PAD_REPEAT_MS          200U

typedef struct {
    gpio_num_t gpio;
    InputKey short_key;
    InputKey long_key;
    bool raw_pressed;
    bool debounced_pressed;
    uint8_t debounce_polls;
    uint32_t press_started_at;
    bool long_press_sent;
    uint32_t last_repeat_at;
} PadButtonState;

static PadButtonState pad_buttons[PAD_BUTTON_COUNT];

static bool pad_button_is_pressed(PadButtonState* btn) {
    return gpio_get_level(btn->gpio) == 0; /* active-low */
}

static void pad_button_init_gpio(gpio_num_t pin) {
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
}

static void pad_button_poll(
    PadButtonState* btn,
    FuriPubSub* pubsub,
    uint32_t now,
    uint32_t long_press_ticks,
    uint32_t repeat_ticks,
    uint32_t* sequence_counter) {
    bool raw = pad_button_is_pressed(btn);

    if(raw == btn->raw_pressed) {
        if(btn->debounce_polls < PAD_DEBOUNCE_POLLS) btn->debounce_polls++;
    } else {
        btn->raw_pressed = raw;
        btn->debounce_polls = 1;
    }

    if(btn->debounce_polls < PAD_DEBOUNCE_POLLS) return;

    if(btn->debounced_pressed == btn->raw_pressed) {
        if(btn->debounced_pressed) {
            uint32_t held = now - btn->press_started_at;
            if(!btn->long_press_sent && held >= long_press_ticks) {
                btn->long_press_sent = true;
                btn->last_repeat_at = now;
                input_publish(pubsub, btn->long_key, InputTypePress, ++(*sequence_counter));
                input_publish(pubsub, btn->long_key, InputTypeLong, *sequence_counter);
            } else if(btn->long_press_sent && (now - btn->last_repeat_at) >= repeat_ticks) {
                btn->last_repeat_at = now;
                input_publish(pubsub, btn->long_key, InputTypeRepeat, *sequence_counter);
            }
        }
        return;
    }

    btn->debounced_pressed = btn->raw_pressed;

    if(btn->debounced_pressed) {
        btn->press_started_at = now;
        btn->long_press_sent = false;
    } else {
        if(btn->long_press_sent) {
            input_publish(pubsub, btn->long_key, InputTypeRelease, *sequence_counter);
        } else {
            input_emit_short(pubsub, btn->short_key, ++(*sequence_counter));
        }
    }
}

static void pad_init(void) {
    const struct {
        gpio_num_t pin;
        InputKey key;
    } cfg[PAD_BUTTON_COUNT] = {
        {BOARD_PIN_BTN_UP, InputKeyUp},
        {BOARD_PIN_BTN_DOWN, InputKeyDown},
        {BOARD_PIN_BTN_LEFT, InputKeyLeft},
        {BOARD_PIN_BTN_RIGHT, InputKeyRight},
        {BOARD_PIN_BTN_OK, InputKeyOk},
        {BOARD_PIN_BTN_BACK, InputKeyBack},
    };

    for(int i = 0; i < PAD_BUTTON_COUNT; i++) {
        pad_button_init_gpio(cfg[i].pin);
        PadButtonState* b = &pad_buttons[i];
        b->gpio = cfg[i].pin;
        b->short_key = cfg[i].key;
        b->long_key = cfg[i].key;
        b->raw_pressed = pad_button_is_pressed(b);
        b->debounced_pressed = b->raw_pressed;
        b->debounce_polls = PAD_DEBOUNCE_POLLS;
        b->press_started_at = 0;
        b->long_press_sent = false;
        b->last_repeat_at = 0;
    }
}

static void pad_poll(FuriPubSub* pubsub, uint32_t* sequence_counter) {
    uint32_t now = furi_get_tick();
    uint32_t long_press_ticks = furi_ms_to_ticks(PAD_LONG_PRESS_MS);
    uint32_t repeat_ticks = furi_ms_to_ticks(PAD_REPEAT_MS);

    for(int i = 0; i < PAD_BUTTON_COUNT; i++) {
        pad_button_poll(
            &pad_buttons[i], pubsub, now, long_press_ticks, repeat_ticks, sequence_counter);
    }
}

#endif /* BOARD_HAS_BUTTONS_PAD */

/* ---- target_input interface ---- */

void target_input_init(void) {
    furi_hal_touch_init();

#if BOARD_HAS_BUTTONS_PAD
    pad_init();
    FURI_LOG_I(TAG, "XPT2046 touch + 6-button pad initialized");
#else
    input_boot_button_init();
    input_boot_button_init_state(&boot_button);
    FURI_LOG_I(TAG, "XPT2046 touch + BOOT button initialized");
#endif

    touch_interaction_active = false;
    touch_zone = TouchZoneNone;
    touch_dir_fired = false;
    touch_back_fired = false;
    touch_started_at = 0;
    touch_repeated_at = 0;
    touch_press_polls = 0;
    touch_release_polls = 0;
    touch_last_up_at = 0;
}

void target_input_poll(FuriPubSub* pubsub, uint32_t* sequence_counter) {
    uint32_t now = furi_get_tick();

#if BOARD_HAS_BUTTONS_PAD
    pad_poll(pubsub, sequence_counter);
#else
    input_boot_button_poll(
        &boot_button,
        pubsub,
        now,
        furi_ms_to_ticks(INPUT_BUTTON_SHORT_PRESS_MAX_MS),
        furi_ms_to_ticks(INPUT_BUTTON_DOUBLE_CLICK_MS),
        sequence_counter);
#endif

    input_touch_poll(pubsub, sequence_counter);
}
