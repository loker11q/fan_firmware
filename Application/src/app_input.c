#include "app_input.h"

#include <string.h>

/* 按键电平保持 30 ms 才算稳定；组合键给第二个键最多 200 ms 时间窗。 */
#define DEBOUNCE_MS 30U
#define COMBO_WINDOW_MS 200U
#define LONG_PRESS_MS 1000U

static void push_event(app_event_t *events, size_t *count,
                       app_event_type_t type, uint32_t now_ms)
{
    /* events 是调用者提供的数组；写入前检查容量，防止数组越界。 */
    if (*count < APP_INPUT_MAX_EVENTS) {
        events[*count].type = type;
        events[*count].timestamp_ms = now_ms;
        (*count)++;
    }
}

static app_event_type_t speed_event(unsigned key)
{
    /* 本文件中的 key 下标 0=KEY1，1=KEY2，2=KEY3。 */
    return key == 0U ? APP_EVENT_SPEED_UP : APP_EVENT_SPEED_DOWN;
}

void app_input_init(app_input_t *input)
{
    if (input != NULL) {
        /* 清空消抖和组合键历史，随后进入主界面。 */
        memset(input, 0, sizeof(*input));
        input->mode = APP_INPUT_MODE_MAIN;
        input->pending_speed_key = -1;
    }
}

void app_input_set_mode(app_input_t *input, app_input_mode_t mode)
{
    if (input == NULL || input->mode == mode) {
        return;
    }
    /* 换界面时丢弃旧界面的半成品组合键判断，避免串键。 */
    input->mode = mode;
    input->pending_speed_key = -1;
    input->pending_speed_emitted = false;
    input->combo_active = false;
    input->combo_enter_emitted = false;
    input->key3_long_emitted = false;
    for (unsigned key = 0U; key < 3U; ++key) {
        /* 若切换时键仍按着，要求先松开再允许新界面响应。 */
        input->button[key].suppress_until_release =
            input->button[key].stable_pressed;
    }
}

static void handle_press(app_input_t *input, unsigned key, uint32_t now_ms,
                         app_event_t *events, size_t *count)
{
    /* 记录“稳定按下”的时刻；后面用当前时间相减识别长按。 */
    app_button_state_t *button = &input->button[key];
    button->pressed_ms = now_ms;

    if (input->mode == APP_INPUT_MODE_MAIN) {
        if (key == 2U) {
            input->key3_long_emitted = false;
            return;
        }

        /* 主界面暂缓 KEY1/KEY2 的单键动作，先看看另一个键是否也按下。 */
        if (input->pending_speed_key >= 0 &&
            input->pending_speed_key != (int8_t)key &&
            (uint32_t)(now_ms - input->pending_speed_ms) > COMBO_WINDOW_MS) {
            if (!input->pending_speed_emitted) {
                push_event(events, count,
                           speed_event((unsigned)input->pending_speed_key), now_ms);
            }
            input->pending_speed_key = (int8_t)key;
            input->pending_speed_ms = now_ms;
            input->pending_speed_emitted = false;
            return;
        }

        if (input->pending_speed_key < 0) {
            input->pending_speed_key = (int8_t)key;
            input->pending_speed_ms = now_ms;
            input->pending_speed_emitted = false;
        } else if (input->pending_speed_key != (int8_t)key &&
                   (uint32_t)(now_ms - input->pending_speed_ms) <= COMBO_WINDOW_MS &&
                   !input->pending_speed_emitted) {
            /* 两键在时间窗内都按下，改为组合键，不再单独加/减档。 */
            input->combo_active = true;
            input->combo_enter_emitted = false;
            input->combo_started_ms = input->pending_speed_ms;
            input->pending_speed_key = -1;
            input->pending_speed_emitted = false;
        }
        return;
    }

    if (input->mode == APP_INPUT_MODE_GAME_MENU) {
        if (key == 0U) {
            push_event(events, count, APP_EVENT_GAME_MENU_PREV, now_ms);
        } else if (key == 1U) {
            push_event(events, count, APP_EVENT_GAME_MENU_NEXT, now_ms);
        }
    } else if (input->mode == APP_INPUT_MODE_GAME) {
        if (key == 0U) {
            push_event(events, count, APP_EVENT_GAME_LEFT, now_ms);
        } else if (key == 1U) {
            push_event(events, count, APP_EVENT_GAME_RIGHT, now_ms);
        }
    }
}

static void handle_release(app_input_t *input, unsigned key, uint32_t now_ms,
                           app_event_t *events, size_t *count)
{
    /* 无符号时间相减可正确处理正常的计时器回绕区间。 */
    const uint32_t held_ms = (uint32_t)(now_ms - input->button[key].pressed_ms);
    if (input->mode == APP_INPUT_MODE_MAIN) {
        if (input->combo_active) {
            const bool both_released = !input->button[0].stable_pressed &&
                                       !input->button[1].stable_pressed;
            if (both_released) {
                /* 短按两键切页；长按的进入游戏事件已在 update 中发送。 */
                if (!input->combo_enter_emitted) {
                    push_event(events, count, APP_EVENT_PAGE_TOGGLE, now_ms);
                }
                input->combo_active = false;
                input->combo_enter_emitted = false;
            }
            return;
        }

        if (key == 2U) {
            if (!input->key3_long_emitted && held_ms < LONG_PRESS_MS) {
                push_event(events, count, APP_EVENT_FAN_TOGGLE, now_ms);
            }
            input->key3_long_emitted = false;
            return;
        }

        if (input->pending_speed_key == (int8_t)key) {
            if (!input->pending_speed_emitted) {
                push_event(events, count, speed_event(key), now_ms);
            }
            input->pending_speed_key = -1;
            input->pending_speed_emitted = false;
        }
        return;
    }

    if (key != 2U || input->key3_long_emitted) {
        return;
    }
    if (held_ms >= LONG_PRESS_MS) {
        push_event(events, count,
                   input->mode == APP_INPUT_MODE_GAME_MENU ?
                   APP_EVENT_GAME_EXIT : APP_EVENT_GAME_BACK,
                   now_ms);
    } else {
        push_event(events, count,
                   input->mode == APP_INPUT_MODE_GAME_MENU ?
                   APP_EVENT_GAME_SELECT : APP_EVENT_GAME_ACTION,
                   now_ms);
    }
}

size_t app_input_update(app_input_t *input,
                        uint8_t pressed_button_mask,
                        uint32_t now_ms,
                        app_event_t out_events[APP_INPUT_MAX_EVENTS])
{
    if (input == NULL || out_events == NULL) {
        return 0U;
    }

    size_t event_count = 0U;
    for (unsigned key = 0U; key < 3U; ++key) {
        app_button_state_t *button = &input->button[key];
        /* 位运算取出某一个按键位：1U << key 只把对应位变成 1；
         * & 按位与用于检查这一位是否为 1。按下位图由硬件层生成。
         */
        const bool raw_pressed =
            (pressed_button_mask & (uint8_t)(1U << key)) != 0U;
        if (raw_pressed != button->raw_pressed) {
            button->raw_pressed = raw_pressed;
            button->raw_changed_ms = now_ms;
        }

        /* 原始状态连续稳定满 30 ms，才更新 stable_pressed 并产生边沿动作。 */
        if (button->stable_pressed != button->raw_pressed &&
            (uint32_t)(now_ms - button->raw_changed_ms) >= DEBOUNCE_MS) {
            button->stable_pressed = button->raw_pressed;
            if (button->stable_pressed) {
                handle_press(input, key, now_ms, out_events, &event_count);
            } else if (button->suppress_until_release) {
                button->suppress_until_release = false;
            } else {
                handle_release(input, key, now_ms, out_events, &event_count);
            }
        }
    }

    /* 已确认组合键后继续按满 1 秒，发送一次“进入游戏”事件。 */
    if (input->mode == APP_INPUT_MODE_MAIN && input->combo_active &&
        !input->combo_enter_emitted && input->button[0].stable_pressed &&
        input->button[1].stable_pressed &&
        (uint32_t)(now_ms - input->combo_started_ms) >= LONG_PRESS_MS) {
        push_event(out_events, &event_count, APP_EVENT_GAME_ENTER, now_ms);
        input->combo_enter_emitted = true;
    }

    /* 单按 KEY1/KEY2 满过组合时间窗后立即发档位事件，不必等到松手。 */
    if (input->mode == APP_INPUT_MODE_MAIN && !input->combo_active &&
        input->pending_speed_key >= 0 && !input->pending_speed_emitted &&
        input->button[(unsigned)input->pending_speed_key].stable_pressed &&
        (uint32_t)(now_ms - input->pending_speed_ms) >= COMBO_WINDOW_MS) {
        push_event(out_events, &event_count,
                   speed_event((unsigned)input->pending_speed_key), now_ms);
        input->pending_speed_emitted = true;
    }

    /* 游戏界面用 KEY3 长按返回；主界面 KEY3 的短按动作在松手时触发。 */
    if (input->button[2].stable_pressed && !input->key3_long_emitted &&
        (uint32_t)(now_ms - input->button[2].pressed_ms) >= LONG_PRESS_MS) {
        input->key3_long_emitted = true;
        if (input->mode == APP_INPUT_MODE_GAME_MENU) {
            push_event(out_events, &event_count, APP_EVENT_GAME_EXIT, now_ms);
        } else if (input->mode == APP_INPUT_MODE_GAME) {
            push_event(out_events, &event_count, APP_EVENT_GAME_BACK, now_ms);
        }
    }

    return event_count;
}
