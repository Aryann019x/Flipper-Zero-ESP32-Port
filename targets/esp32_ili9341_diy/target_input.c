/**
 * @file target_input.c
 * Input driver: 5-way pad + BOOT-as-BACK for esp32_ili9341_diy
 *
 * Wiring (active-low to GND):
 *   UP=34 (input-only, EXT 10k pull-up REQUIRED, no internal pull-up)
 *   DOWN=26, LEFT=33, RIGHT=27, OK=35 (input-only, EXT 10k pull-up REQUIRED)
 *   BACK=0 (BOOT button)
 *
 * Fallback: long-press OK also emits BACK so the UI is usable even if
 * BOOT is inconvenient to reach.
 */

#include "target_input.h"

#include <furi_hal_resources.h>
#include <boards/board.h>
#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_rom_sys.h>

#define TAG "InputButtons"

#define INPUT_DEBOUNCE_POLLS   2U
#define INPUT_LONG_PRESS_MS    500U
#define INPUT_REPEAT_MS        200U

typedef struct {
    gpio_num_t gpio;
    bool       inverted;
    bool       use_internal_pullup; /* false for GPIO34/35 (input-only) */
    InputKey   short_key;
    InputKey   long_key;
    bool       emit_back_on_long; /* OK long -> BACK fallback */
    bool       raw_pressed;
    bool       debounced_pressed;
    uint8_t    debounce_polls;
    uint32_t   press_started_at;
    bool       long_press_sent;
    uint32_t   last_repeat_at;
} ButtonState;

#define NUM_BUTTONS 6

static ButtonState buttons[NUM_BUTTONS];

static const char* input_key_name(InputKey key) {
    switch(key) {
    case InputKeyUp:    return "UP";
    case InputKeyDown:  return "DOWN";
    case InputKeyLeft:  return "LEFT";
    case InputKeyRight: return "RIGHT";
    case InputKeyOk:    return "OK";
    case InputKeyBack:  return "BACK";
    default:            return "?";
    }
}

static const char* input_type_name(InputType type) {
    switch(type) {
    case InputTypePress:   return "press";
    case InputTypeRelease: return "release";
    case InputTypeShort:   return "short";
    case InputTypeLong:    return "long";
    case InputTypeRepeat:  return "repeat";
    default:               return "?";
    }
}

static void input_publish(FuriPubSub* pubsub, InputKey key, InputType type, uint32_t sequence) {
    InputEvent event = {
        .sequence_source  = INPUT_SEQUENCE_SOURCE_HARDWARE,
        .sequence_counter = sequence,
        .key              = key,
        .type             = type,
    };
    esp_rom_printf("[BTN] %s %s\r\n", input_key_name(key), input_type_name(type));
    furi_pubsub_publish(pubsub, &event);
}

static void input_emit_short(FuriPubSub* pubsub, InputKey key, uint32_t sequence) {
    input_publish(pubsub, key, InputTypePress,   sequence);
    input_publish(pubsub, key, InputTypeShort,   sequence);
    input_publish(pubsub, key, InputTypeRelease, sequence);
}

static bool button_is_pressed(ButtonState* btn) {
    int level = gpio_get_level(btn->gpio);
    return btn->inverted ? (level == 0) : (level != 0);
}

static void button_init_gpio(gpio_num_t pin, bool pull_up) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = pull_up ? GPIO_PULLUP_ENABLE   : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if(err != ESP_OK) {
        FURI_LOG_E(TAG, "GPIO %d config failed: %s", pin, esp_err_to_name(err));
    }
}

static void button_poll(
    ButtonState* btn,
    FuriPubSub*  pubsub,
    uint32_t     now,
    uint32_t     long_press_ticks,
    uint32_t     repeat_ticks,
    uint32_t*    sequence_counter)
{
    bool raw = button_is_pressed(btn);

    if(raw == btn->raw_pressed) {
        if(btn->debounce_polls < INPUT_DEBOUNCE_POLLS) btn->debounce_polls++;
    } else {
        btn->raw_pressed   = raw;
        btn->debounce_polls = 1;
    }

    if(btn->debounce_polls < INPUT_DEBOUNCE_POLLS) return;

    if(btn->debounced_pressed == btn->raw_pressed) {
        if(btn->debounced_pressed) {
            uint32_t held = now - btn->press_started_at;            if(!btn->long_press_sent && held >= long_press_ticks) {
                btn->long_press_sent = true;
                btn->last_repeat_at  = now;
                if(btn->emit_back_on_long) {
                    /* OK/LEFT long -> BACK fallback: emit a real BACK click
                     * (Press+Short) so every view reacts exactly like it
                     * would to the physical BACK key, and keep clicking
                     * while held. */
                    input_publish(pubsub, InputKeyBack, InputTypePress, ++(*sequence_counter));
                    input_publish(pubsub, InputKeyBack, InputTypeShort, *sequence_counter);
                } else {
                    input_publish(pubsub, btn->long_key, InputTypePress, ++(*sequence_counter));
                    input_publish(pubsub, btn->long_key, InputTypeLong,  *sequence_counter);
                }
            } else if(btn->long_press_sent && (now - btn->last_repeat_at) >= repeat_ticks) {
                btn->last_repeat_at  = now;
                if(btn->emit_back_on_long) {
                    input_publish(pubsub, InputKeyBack, InputTypeShort, *sequence_counter);
                } else {
                    input_publish(pubsub, btn->long_key, InputTypeRepeat, *sequence_counter);
                }
            }
        }
        return;
    }

    btn->debounced_pressed = btn->raw_pressed;

    if(btn->debounced_pressed) {
        btn->press_started_at = now;
        btn->long_press_sent  = false;
    } else {
        if(btn->long_press_sent) {
            InputKey lk = (btn->emit_back_on_long) ? InputKeyBack : btn->long_key;
            input_publish(pubsub, lk, InputTypeRelease, *sequence_counter);
        } else {
            input_emit_short(pubsub, btn->short_key, ++(*sequence_counter));
        }
    }
}

void target_input_init(void) {
    /* {gpio, internal_pullup, short_key, long_key, ok_long_gives_back} */
    const struct {
        gpio_num_t pin;
        bool pullup;
        InputKey sk;
        InputKey lk;
        bool back_on_long;
    } cfg[NUM_BUTTONS] = {
        { BOARD_PIN_BTN_UP,    false, InputKeyUp,    InputKeyUp,    false },
        { BOARD_PIN_BTN_DOWN,  true,  InputKeyDown,  InputKeyDown,  false },
        /* LEFT long = BACK fallback (like OK long) — user preference */
        { BOARD_PIN_BTN_LEFT,  true,  InputKeyLeft,  InputKeyLeft,  true  },
        { BOARD_PIN_BTN_RIGHT, true,  InputKeyRight, InputKeyRight, false },
        /* OK on GPIO35: input-only, external pull-up required */
        { BOARD_PIN_BTN_OK,    false, InputKeyOk,    InputKeyOk,    true  },
        { BOARD_PIN_BTN_BACK,  true,  InputKeyBack,  InputKeyBack,  false },
    };

    for(int i = 0; i < NUM_BUTTONS; i++) {
        button_init_gpio(cfg[i].pin, cfg[i].pullup);

        ButtonState* b      = &buttons[i];
        b->gpio             = cfg[i].pin;
        b->inverted         = true;
        b->use_internal_pullup = cfg[i].pullup;
        b->short_key        = cfg[i].sk;
        b->long_key         = cfg[i].lk;
        b->emit_back_on_long = cfg[i].back_on_long;
        b->raw_pressed      = button_is_pressed(b);
        b->debounced_pressed = b->raw_pressed;
        b->debounce_polls   = INPUT_DEBOUNCE_POLLS;
        b->press_started_at = 0;
        b->long_press_sent  = false;
        b->last_repeat_at   = 0;
    }

    FURI_LOG_I(TAG, "5-way + BACK input initialized (esp32_ili9341_diy)");
    FURI_LOG_I(TAG, "BACK = BOOT key, or hold LEFT/OK ~0.5s");
    FURI_LOG_I(TAG, "NOTE: GPIO34/35 need external 10k pull-ups to 3V3");
}

void target_input_poll(FuriPubSub* pubsub, uint32_t* sequence_counter) {
    uint32_t now              = furi_get_tick();
    uint32_t long_press_ticks = furi_ms_to_ticks(INPUT_LONG_PRESS_MS);
    uint32_t repeat_ticks     = furi_ms_to_ticks(INPUT_REPEAT_MS);

    for(int i = 0; i < NUM_BUTTONS; i++) {
        button_poll(&buttons[i], pubsub, now, long_press_ticks, repeat_ticks, sequence_counter);
    }
}
