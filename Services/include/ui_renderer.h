#ifndef UI_RENDERER_H
#define UI_RENDERER_H

#include "app_status.h"
#include "game_engine.h"

#include <stdint.h>

/* renderer（渲染器）只决定画什么；像素/文字最终由 Hardware 层绘制。
 * status/game 都是只读指针，渲染过程中不会改变业务状态。
 */
void ui_renderer_draw_main(const app_public_status_t *status,
                           uint8_t page,
                           const char *notice);
void ui_renderer_draw_game_menu(const game_engine_t *game);
void ui_renderer_draw_game(const game_engine_t *game);

#endif
