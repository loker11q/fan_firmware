#include "ui_renderer.h"
#include "hardware_port.h"

#include <stdio.h>

/* 将游戏编号转换成屏幕上显示的中文名称。 */
static const char *game_name(game_id_t id)
{
    switch (id) {
    case GAME_ID_SNAKE: return "贪吃蛇";
    case GAME_ID_REACTION: return "反应测试";
    case GAME_ID_GUESS: return "猜数字";
    default: return "未知游戏";
    }
}

static const char *game_name_en(game_id_t id)
{
    switch (id) {
    case GAME_ID_SNAKE: return "SNAKE";
    case GAME_ID_REACTION: return "REACTION";
    case GAME_ID_GUESS: return "GUESS";
    default: return "GAME";
    }
}

void ui_renderer_draw_main(const app_public_status_t *status,
                           uint8_t page,
                           const char *notice)
{
    /* status 是只读快照指针；OLED 不可用时直接返回，不影响风扇控制。 */
    if (status == 0 || !hw_display_available()) {
        return;
    }
    hw_display_clear();
    /* line 是临时文本缓冲区；snprintf 限制写入长度，防止写出数组边界。 */
    char line[40];

    /* page 的最低位决定显示风扇页还是系统页。 */
    if ((page & 1U) == 0U) {
        if (notice != 0 && notice[0] != '\0') {
            hw_display_text(0U, 0U, notice);
        } else {
            (void)snprintf(line, sizeof(line), "风扇 %s",
                           status->fan_configured ?
                           (status->fan_running ? "RUN" : "STOP") : "未配置");
            hw_display_text(0U, 0U, line);
        }
        (void)snprintf(line, sizeof(line), "档位 %u/10 PWM %u%%",
                       (unsigned)status->speed_level,
                       (unsigned)status->duty_percent);
        hw_display_text(0U, 16U, line);
        if (status->rpm_valid) {
            (void)snprintf(line, sizeof(line), "转速 %lu RPM",
                           (unsigned long)status->rpm);
        } else {
            (void)snprintf(line, sizeof(line), "转速 未配置");
        }
        hw_display_text(0U, 32U, line);
        if (status->battery_valid) {
            (void)snprintf(line, sizeof(line), "电池 %u.%03u V%s",
                           (unsigned)(status->battery_mv / 1000U),
                           (unsigned)(status->battery_mv % 1000U),
                           status->low_voltage_warning ? " LOW" : "");
        } else {
            (void)snprintf(line, sizeof(line), "电池读取错误");
        }
        hw_display_text(0U, 48U, line);
    } else {
        hw_display_text(0U, 0U, "系统 SYS");
        if (status->wifi_connected) {
            hw_display_text(0U, 16U, "WiFi 已连接");
            (void)snprintf(line, sizeof(line), "IP %s", status->ip);
        } else {
            hw_display_text(0U, 16U, "WiFi 离线");
            (void)snprintf(line, sizeof(line), "IP --");
        }
        hw_display_text(0U, 32U, line);
        (void)snprintf(line, sizeof(line), "FW %.8s OTA %.6s",
                       status->firmware_version, status->ota_state);
        hw_display_text(0U, 48U, line);
        if (status->last_error[0] != '\0') {
            (void)snprintf(line, sizeof(line), "ERR %.16s", status->last_error);
        } else {
            (void)snprintf(line, sizeof(line), "ERR none");
        }
        hw_display_text(0U, 56U, line);
    }
    (void)hw_display_flush();
}

void ui_renderer_draw_game_menu(const game_engine_t *game)
{
    /* 菜单渲染只绘图，不在这里改变 game 状态。 */
    if (game == 0 || !hw_display_available()) {
        return;
    }
    hw_display_clear();
    hw_display_text(0U, 0U, "小游戏 GAMES");
    hw_display_text(0U, 16U, "> ");
    hw_display_text(16U, 16U, game_name(game->selected));
    hw_display_text(0U, 32U, game_name_en(game->selected));
    hw_display_text(0U, 48U, "KEY1/2选 KEY3开");
    (void)hw_display_flush();
}

void ui_renderer_draw_game(const game_engine_t *game)
{
    /* 根据 active 游戏编号画对应画面；按键规则由 Application 层处理。 */
    if (game == 0 || !hw_display_available()) {
        return;
    }
    char line[40];
    hw_display_clear();
    if (game->active == GAME_ID_SNAKE) {
        hw_display_text(0U, 0U, "贪吃蛇 SNAKE");
        /* 16 列 × 8 像素 = 128 像素宽；8 行 × 6 像素 = 48 像素高。 */
        const uint8_t cell_w = 8U;
        const uint8_t cell_h = 6U;
        for (uint8_t i = 0U; i < game->snake_length; ++i) {
            hw_display_fill_rect((uint8_t)(game->snake_x[i] * cell_w),
                                 (uint8_t)(16U + game->snake_y[i] * cell_h),
                                 (uint8_t)(cell_w - 1U),
                                 (uint8_t)(cell_h - 1U));
        }
        hw_display_fill_rect((uint8_t)(game->food_x * cell_w),
                             (uint8_t)(16U + game->food_y * cell_h), 5U, 4U);
        if (game->phase == GAME_PHASE_PAUSED) {
            hw_display_text(0U, 48U, "暂停 PAUSE");
        } else if (game->phase == GAME_PHASE_OVER) {
            hw_display_text(0U, 48U, "结束 OVER");
        }
    } else if (game->active == GAME_ID_REACTION) {
        hw_display_text(0U, 0U, "反应测试 REACT");
        if (game->phase == GAME_PHASE_WAITING) {
            hw_display_text(0U, 24U, "等待 WAIT...");
        } else if (game->phase == GAME_PHASE_READY) {
            hw_display_text(0U, 24U, "按任意键 GO!");
        } else if (game->phase == GAME_PHASE_RESULT && game->false_start) {
            hw_display_text(0U, 24U, "太早 FALSE START");
        } else if (game->phase == GAME_PHASE_RESULT) {
            (void)snprintf(line, sizeof(line), "%lu ms",
                           (unsigned long)game->reaction_ms);
            hw_display_text(0U, 24U, line);
            hw_display_text(0U, 40U, "KEY3再试 RETRY");
        }
    } else {
        hw_display_text(0U, 0U, "猜数字 GUESS");
        (void)snprintf(line, sizeof(line), "%u  [%u-%u]",
                       (unsigned)game->guess_value,
                       (unsigned)game->guess_low,
                       (unsigned)game->guess_high);
        hw_display_text(0U, 24U, line);
        if (game->phase == GAME_PHASE_RESULT && game->won) {
            hw_display_text(0U, 40U, "猜对了! CORRECT");
        } else if (game->guess_hint > 0) {
            hw_display_text(0U, 40U, "再大 HIGH");
        } else if (game->guess_hint < 0) {
            hw_display_text(0U, 40U, "再小 LOW");
        }
        hw_display_text(0U, 56U, "KEY1-/KEY2+ KEY3确认");
    }
    (void)hw_display_flush();
}
