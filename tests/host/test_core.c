#include "app_input.h"
#include "battery_math.h"
#include "fan_core.h"
#include "game_engine.h"
#include "oled_font.h"

#include <assert.h>
#include <stdio.h>

/* 这些是电脑上的纯 C 测试，不访问 ESP32 外设。
 * assert 条件不成立时程序会立即失败，帮助定位哪条规则不符合预期。
 */
static size_t update_input(app_input_t *input,
                           uint8_t mask,
                           uint32_t now,
                           app_event_t *events)
{
    return app_input_update(input, mask, now, events);
}

static void test_fan_levels(void)
{
    /* 检查默认停机、10 档边界、PWM 换算及低压保护。 */
    const fan_config_t config = {
        .motor_enabled = true,
        .pwm_frequency_hz = 25000U,
        .max_duty_percent = 80U,
        .pwm_active_low = false,
        .pulses_per_revolution = 2U,
        .battery_warn_mv = 0U,
        .battery_stop_mv = 0U,
    };
    fan_core_t fan;
    fan_core_init(&fan, &config);
    assert(fan_core_is_configured(&fan));
    assert(fan_core_target_duty(&fan) == 0U);
    assert(fan_core_set_running(&fan, true));
    assert(fan_core_target_duty(&fan) == 8U);

    fan_core_update_battery(&fan, false, 0U);
    assert(fan.running); /* No stop threshold is configured in this baseline. */

    for (unsigned i = 0U; i < 20U; ++i) {
        fan_core_adjust_level(&fan, 1);
    }
    assert(fan.speed_level == FAN_SPEED_LEVELS);
    assert(fan_core_target_duty(&fan) == 80U);
    fan_core_adjust_level(&fan, 1);
    assert(fan.speed_level == FAN_SPEED_LEVELS);
    for (unsigned i = 0U; i < 20U; ++i) {
        fan_core_adjust_level(&fan, -1);
    }
    assert(fan.speed_level == 1U);
    assert(fan_core_target_duty(&fan) == 8U);
    assert(fan_core_set_running(&fan, false));
    assert(fan_core_target_duty(&fan) == 0U);

    fan_config_t protected_config = config;
    protected_config.battery_stop_mv = 6500U;
    fan_core_init(&fan, &protected_config);
    assert(fan_core_set_running(&fan, true));
    fan_core_update_battery(&fan, false, 0U);
    assert(!fan.running);
    assert(fan.low_voltage_stop);
    assert(fan_core_target_duty(&fan) == 0U);

    fan_config_t disabled = config;
    disabled.motor_enabled = false;
    fan_core_init(&fan, &disabled);
    assert(!fan_core_is_configured(&fan));
    assert(!fan_core_set_running(&fan, true));
    assert(fan_core_target_duty(&fan) == 0U);
}

static void test_key_debounce_and_actions(void)
{
    /* 输入时间点模拟按键抖动/按下/松开，不需要真实按键。 */
    app_input_t input;
    app_event_t events[APP_INPUT_MAX_EVENTS];
    app_input_init(&input);

    assert(update_input(&input, 0x01U, 0U, events) == 0U);
    assert(update_input(&input, 0x01U, 30U, events) == 0U);
    size_t count = update_input(&input, 0x00U, 50U, events);
    assert(count == 0U);
    count = update_input(&input, 0x00U, 80U, events);
    assert(count == 1U);
    assert(events[0].type == APP_EVENT_SPEED_UP);

    app_input_init(&input);
    (void)update_input(&input, 0x02U, 0U, events);
    (void)update_input(&input, 0x02U, 30U, events);
    (void)update_input(&input, 0x00U, 50U, events);
    count = update_input(&input, 0x00U, 80U, events);
    assert(count == 1U);
    assert(events[0].type == APP_EVENT_SPEED_DOWN);

    app_input_init(&input);
    (void)update_input(&input, 0x04U, 0U, events);
    (void)update_input(&input, 0x04U, 30U, events);
    (void)update_input(&input, 0x00U, 50U, events);
    count = update_input(&input, 0x00U, 80U, events);
    assert(count == 1U);
    assert(events[0].type == APP_EVENT_FAN_TOGGLE);
}

static void test_combo_page_and_game_entry(void)
{
    /* 验证两键短按切页、长按进入游戏菜单，以及模式切换时抑制旧按键。 */
    app_input_t input;
    app_event_t events[APP_INPUT_MAX_EVENTS];
    app_input_init(&input);
    (void)update_input(&input, 0x03U, 0U, events);
    (void)update_input(&input, 0x03U, 30U, events);
    (void)update_input(&input, 0x00U, 50U, events);
    const size_t count = update_input(&input, 0x00U, 80U, events);
    assert(count == 1U);
    assert(events[0].type == APP_EVENT_PAGE_TOGGLE);

    app_input_init(&input);
    (void)update_input(&input, 0x03U, 0U, events);
    (void)update_input(&input, 0x03U, 30U, events);
    const size_t long_count = update_input(&input, 0x03U, 1030U, events);
    assert(long_count == 1U);
    assert(events[0].type == APP_EVENT_GAME_ENTER);
    app_input_set_mode(&input, APP_INPUT_MODE_GAME_MENU);
    (void)update_input(&input, 0x00U, 1040U, events);
    const size_t release_count = update_input(&input, 0x00U, 1070U, events);
    assert(release_count == 0U);
}

static void test_game_menu_and_game_key_events(void)
{
    /* 验证游戏菜单选择、确认和游戏中的方向键事件。 */
    app_input_t input;
    app_event_t events[APP_INPUT_MAX_EVENTS];
    app_input_init(&input);
    app_input_set_mode(&input, APP_INPUT_MODE_GAME_MENU);
    (void)update_input(&input, 0x00U, 0U, events);
    size_t count = update_input(&input, 0x02U, 10U, events);
    assert(count == 0U);
    count = update_input(&input, 0x02U, 40U, events);
    assert(count == 1U);
    assert(events[0].type == APP_EVENT_GAME_MENU_NEXT);

    (void)update_input(&input, 0x00U, 50U, events);
    (void)update_input(&input, 0x00U, 80U, events);
    (void)update_input(&input, 0x04U, 100U, events);
    (void)update_input(&input, 0x04U, 130U, events);
    (void)update_input(&input, 0x00U, 150U, events);
    count = update_input(&input, 0x00U, 180U, events);
    assert(count == 1U);
    assert(events[0].type == APP_EVENT_GAME_SELECT);

    app_input_set_mode(&input, APP_INPUT_MODE_GAME);
    (void)update_input(&input, 0x00U, 200U, events);
    (void)update_input(&input, 0x01U, 210U, events);
    count = update_input(&input, 0x01U, 240U, events);
    assert(count == 1U);
    assert(events[0].type == APP_EVENT_GAME_LEFT);
}

static void test_battery_and_rpm_math(void)
{
    /* 使用已知输入值检查 ADC 分压换算和脉冲到 RPM 的公式。 */
    assert(battery_math_adc_to_system_mv(1408U) == 7399U);
    assert(battery_math_adc_to_system_mv(0U) == 0U);
    assert(battery_math_adc_to_system_mv(UINT16_MAX) == UINT16_MAX);

    uint32_t rpm = 0U;
    assert(battery_math_pulses_to_rpm(4U, 1000U, 2U, &rpm));
    assert(rpm == 120U);
    assert(!battery_math_pulses_to_rpm(1U, 0U, 2U, &rpm));
    assert(!battery_math_pulses_to_rpm(1U, 1000U, 0U, &rpm));
    assert(!battery_math_pulses_to_rpm(1U, 1000U, 2U, NULL));
}

static void test_oled_font_lookup(void)
{
    /* 字模数组在程序里；这里只检查查询成功/失败，不连接显示屏。 */
    uint8_t ascii[5] = { 0 };
    uint8_t chinese[32] = { 0 };
    assert(oled_font_get_ascii('A', ascii));
    assert(oled_font_get_chinese(0x98CEU, chinese)); /* 风 */
    assert(!oled_font_get_chinese(0xFFFFU, chinese));
}

static void test_guess_boundaries(void)
{
    /* 数字不能越过当前上下限，猜中后应进入结果阶段。 */
    game_engine_t game;
    game_engine_init(&game, 123U);
    game.selected = GAME_ID_GUESS;
    game_engine_start(&game, 0U);
    assert(game.phase == GAME_PHASE_PLAYING);
    assert(game.guess_low == 1U && game.guess_high == 99U);

    game.guess_value = game.guess_low;
    game_engine_input(&game, GAME_INPUT_LEFT, 1U);
    assert(game.guess_value == 1U);
    game.guess_value = game.guess_high;
    game_engine_input(&game, GAME_INPUT_RIGHT, 2U);
    assert(game.guess_value == 99U);

    game.guess_value = 50U;
    game.guess_target = 50U;
    game_engine_input(&game, GAME_INPUT_ACTION, 3U);
    assert(game.phase == GAME_PHASE_RESULT);
    assert(game.won);
}

static void test_snake_edges_and_reaction_timer(void)
{
    /* 检查蛇撞墙结束、反应计时、提前按键和重试状态。 */
    game_engine_t game;
    game_engine_init(&game, 7U);
    game.selected = GAME_ID_SNAKE;
    game_engine_start(&game, 100U);
    assert(game.snake_length == 3U);
    game.snake_x[0] = (uint8_t)(GAME_GRID_WIDTH - 1U);
    game.snake_y[0] = 4U;
    game.snake_direction = 1U; /* right */
    game.next_step_ms = 101U;
    game_engine_tick(&game, 101U);
    assert(game.phase == GAME_PHASE_OVER);

    game_engine_init(&game, 9U);
    game.selected = GAME_ID_REACTION;
    game_engine_start(&game, 500U);
    assert(game.phase == GAME_PHASE_WAITING);
    const uint32_t ready_at = game.deadline_ms;
    game_engine_tick(&game, ready_at - 1U);
    assert(game.phase == GAME_PHASE_WAITING);
    game_engine_tick(&game, ready_at);
    assert(game.phase == GAME_PHASE_READY);
    game_engine_input(&game, GAME_INPUT_ACTION, ready_at + 137U);
    assert(game.phase == GAME_PHASE_RESULT);
    assert(game.reaction_ms == 137U);
    assert(!game.false_start);

    game_engine_input(&game, GAME_INPUT_ACTION, ready_at + 200U);
    assert(game.phase == GAME_PHASE_WAITING);
    game_engine_input(&game, GAME_INPUT_LEFT, ready_at + 201U);
    assert(game.phase == GAME_PHASE_RESULT);
    assert(game.false_start);
}

int main(void)
{
    /* 主机测试入口：逐项运行，全部通过才输出成功提示并返回 0。 */
    test_fan_levels();
    test_key_debounce_and_actions();
    test_combo_page_and_game_entry();
    test_game_menu_and_game_key_events();
    test_battery_and_rpm_math();
    test_oled_font_lookup();
    test_guess_boundaries();
    test_snake_edges_and_reaction_timer();
    puts("All host core tests passed.");
    return 0;
}
