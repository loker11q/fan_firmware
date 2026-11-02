#include "game_engine.h"

#include <stddef.h>
#include <string.h>

#define SNAKE_STEP_MS 300U
#define REACTION_MIN_MS 1000U
#define REACTION_RANGE_MS 2001U
#define GUESS_MIN 1U
#define GUESS_MAX 99U

/* 方向编号在蛇的移动和转向计算中循环使用：上、右、下、左。 */
enum { DIR_UP = 0U, DIR_RIGHT, DIR_DOWN, DIR_LEFT };

/* xorshift 伪随机数：不需要动态内存，状态只保存在 game->rng_state。 */
static uint32_t random_next(game_engine_t *game)
{
    uint32_t x = game->rng_state;
    if (x == 0U) {
        x = 0x6D2B79F5U;
    }
    /* << / >> 是位移，^ 是异或；组合后快速生成伪随机序列，不用于密码学。 */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    game->rng_state = x;
    return x;
}

static bool point_is_on_snake(const game_engine_t *game, uint8_t x, uint8_t y)
{
    /* 蛇身第 0 节是头；后面的数组元素依次是身体。 */
    for (uint8_t i = 0U; i < game->snake_length; ++i) {
        if (game->snake_x[i] == x && game->snake_y[i] == y) {
            return true;
        }
    }
    return false;
}

static void snake_place_food(game_engine_t *game)
{
    if (game->snake_length >= GAME_SNAKE_CAPACITY) {
        game->phase = GAME_PHASE_OVER;
        return;
    }
    /* 随机尝试空格；若棋盘已满或尝试完仍找不到空位，则结束游戏。 */
    for (unsigned attempt = 0U; attempt < GAME_GRID_WIDTH * GAME_GRID_HEIGHT; ++attempt) {
        const uint8_t x = (uint8_t)(random_next(game) % GAME_GRID_WIDTH);
        const uint8_t y = (uint8_t)(random_next(game) % GAME_GRID_HEIGHT);
        if (!point_is_on_snake(game, x, y)) {
            game->food_x = x;
            game->food_y = y;
            return;
        }
    }
    game->phase = GAME_PHASE_OVER;
}

static void snake_start(game_engine_t *game, uint32_t now_ms)
{
    /* 建立长度为 3 的初始蛇，并记录下一步时间，不在这里延时等待。 */
    game->phase = GAME_PHASE_PLAYING;
    game->snake_length = 3U;
    game->snake_direction = DIR_RIGHT;
    game->snake_x[0] = 7U;
    game->snake_y[0] = 4U;
    game->snake_x[1] = 6U;
    game->snake_y[1] = 4U;
    game->snake_x[2] = 5U;
    game->snake_y[2] = 4U;
    game->next_step_ms = now_ms + SNAKE_STEP_MS;
    snake_place_food(game);
}

static void reaction_start(game_engine_t *game, uint32_t now_ms)
{
    /* 随机等待 1～3 秒；tick 到时间后才显示 GO，提前按键算抢按。 */
    game->phase = GAME_PHASE_WAITING;
    game->false_start = false;
    game->reaction_ms = 0U;
    game->deadline_ms = now_ms + REACTION_MIN_MS +
                        (random_next(game) % REACTION_RANGE_MS);
}

static void guess_start(game_engine_t *game)
{
    /* 目标数字在 1～99 之间；guess_value 是玩家当前正在选的数字。 */
    game->phase = GAME_PHASE_PLAYING;
    game->guess_low = GUESS_MIN;
    game->guess_high = GUESS_MAX;
    game->guess_value = 50U;
    game->guess_target = (uint8_t)(GUESS_MIN +
                          random_next(game) % (GUESS_MAX - GUESS_MIN + 1U));
    game->guess_hint = 0;
    game->won = false;
}

void game_engine_init(game_engine_t *game, uint32_t seed)
{
    if (game == NULL) {
        return;
    }
    /* 所有游戏状态放在调用者提供的结构体中，清零后设置初始菜单。 */
    memset(game, 0, sizeof(*game));
    game->phase = GAME_PHASE_MENU;
    game->selected = GAME_ID_SNAKE;
    game->rng_state = seed == 0U ? 0xA341316CU : seed;
}

void game_engine_menu_move(game_engine_t *game, int8_t direction)
{
    if (game == NULL || game->phase != GAME_PHASE_MENU || direction == 0) {
        return;
    }
    /* 转成 int 临时计算，才能安全判断小于 0，再回写枚举类型。 */
    const int count = (int)GAME_ID_COUNT;
    int selected = (int)game->selected + (direction > 0 ? 1 : -1);
    if (selected < 0) {
        selected = count - 1;
    } else if (selected >= count) {
        selected = 0;
    }
    game->selected = (game_id_t)selected;
}

void game_engine_start(game_engine_t *game, uint32_t now_ms)
{
    if (game == NULL || game->selected >= GAME_ID_COUNT) {
        return;
    }
    /* selected 是菜单选项；active 记录真正开始运行的游戏。 */
    game->active = game->selected;
    switch (game->active) {
    case GAME_ID_SNAKE:
        snake_start(game, now_ms);
        break;
    case GAME_ID_REACTION:
        reaction_start(game, now_ms);
        break;
    case GAME_ID_GUESS:
        guess_start(game);
        break;
    default:
        game->phase = GAME_PHASE_MENU;
        break;
    }
}

static void snake_turn(game_engine_t *game, bool left)
{
    /* 四个方向组成环：左转加 3 再取模，右转加 1 再取模。 */
    game->snake_direction = left ?
        (uint8_t)((game->snake_direction + 3U) % 4U) :
        (uint8_t)((game->snake_direction + 1U) % 4U);
}

void game_engine_input(game_engine_t *game, game_input_t input, uint32_t now_ms)
{
    if (game == NULL) {
        return;
    }
    /* 先按游戏类型分支；同一个按键在不同游戏中的含义不同。 */
    if (game->active == GAME_ID_REACTION) {
        if (game->phase == GAME_PHASE_WAITING) {
            /* 倒计时未结束就按下，标记为抢按。 */
            game->false_start = true;
            game->phase = GAME_PHASE_RESULT;
        } else if (game->phase == GAME_PHASE_READY) {
            /* deadline_ms 在进入 READY 时被设置为起点时间。 */
            game->reaction_ms = now_ms - game->deadline_ms;
            game->phase = GAME_PHASE_RESULT;
        } else if (game->phase == GAME_PHASE_RESULT &&
                   input == GAME_INPUT_ACTION) {
            reaction_start(game, now_ms);
        }
        return;
    }

    if (game->active == GAME_ID_SNAKE) {
        if (game->phase == GAME_PHASE_PLAYING) {
            if (input == GAME_INPUT_LEFT) {
                snake_turn(game, true);
            } else if (input == GAME_INPUT_RIGHT) {
                snake_turn(game, false);
            } else if (input == GAME_INPUT_ACTION) {
                game->phase = GAME_PHASE_PAUSED;
            }
        } else if (game->phase == GAME_PHASE_PAUSED &&
                   input == GAME_INPUT_ACTION) {
            game->phase = GAME_PHASE_PLAYING;
        } else if (game->phase == GAME_PHASE_OVER &&
                   input == GAME_INPUT_ACTION) {
            snake_start(game, now_ms);
        }
        return;
    }

    if (game->active == GAME_ID_GUESS && game->phase == GAME_PHASE_PLAYING) {
        if (input == GAME_INPUT_LEFT && game->guess_value > game->guess_low) {
            game->guess_value--;
        } else if (input == GAME_INPUT_RIGHT &&
                   game->guess_value < game->guess_high) {
            game->guess_value++;
        } else if (input == GAME_INPUT_ACTION) {
            /* 猜中就结束；否则缩小范围，让下一次选择更容易。 */
            if (game->guess_value == game->guess_target) {
                game->won = true;
                game->phase = GAME_PHASE_RESULT;
                return;
            }
            if (game->guess_value < game->guess_target) {
                game->guess_low = (uint8_t)(game->guess_value + 1U);
                game->guess_hint = 1;
            } else {
                game->guess_high = (uint8_t)(game->guess_value - 1U);
                game->guess_hint = -1;
            }
            if (game->guess_value < game->guess_low) {
                game->guess_value = game->guess_low;
            } else if (game->guess_value > game->guess_high) {
                game->guess_value = game->guess_high;
            }
        }
    } else if (game->active == GAME_ID_GUESS &&
               game->phase == GAME_PHASE_RESULT &&
               input == GAME_INPUT_ACTION) {
        guess_start(game);
    }
}

static void snake_step(game_engine_t *game)
{
    /* 先算出下一格，再检查边界/碰撞，确认安全后才整体移动蛇身。 */
    uint8_t next_x = game->snake_x[0];
    uint8_t next_y = game->snake_y[0];
    switch (game->snake_direction) {
    case DIR_UP:
        if (next_y == 0U) { game->phase = GAME_PHASE_OVER; return; }
        next_y--;
        break;
    case DIR_RIGHT:
        if ((uint8_t)(next_x + 1U) >= GAME_GRID_WIDTH) { game->phase = GAME_PHASE_OVER; return; }
        next_x++;
        break;
    case DIR_DOWN:
        if ((uint8_t)(next_y + 1U) >= GAME_GRID_HEIGHT) { game->phase = GAME_PHASE_OVER; return; }
        next_y++;
        break;
    case DIR_LEFT:
        if (next_x == 0U) { game->phase = GAME_PHASE_OVER; return; }
        next_x--;
        break;
    default:
        game->phase = GAME_PHASE_OVER;
        return;
    }

    /* 吃到食物时尾巴不移走，蛇长度增加；否则尾巴本步会离开旧位置。 */
    const bool eating = next_x == game->food_x && next_y == game->food_y;
    const uint8_t collision_length = eating ? game->snake_length :
                                              (uint8_t)(game->snake_length - 1U);
    for (uint8_t i = 0U; i < collision_length; ++i) {
        if (game->snake_x[i] == next_x && game->snake_y[i] == next_y) {
            game->phase = GAME_PHASE_OVER;
            return;
        }
    }

    uint8_t new_length = game->snake_length;
    if (eating && new_length < GAME_SNAKE_CAPACITY) {
        new_length++;
    }
    /* 从尾到头拷贝，防止前一格被覆盖后丢失。 */
    for (uint8_t i = (uint8_t)(new_length - 1U); i > 0U; --i) {
        game->snake_x[i] = game->snake_x[i - 1U];
        game->snake_y[i] = game->snake_y[i - 1U];
    }
    game->snake_x[0] = next_x;
    game->snake_y[0] = next_y;
    game->snake_length = new_length;
    if (eating) {
        snake_place_food(game);
    }
}

void game_engine_tick(game_engine_t *game, uint32_t now_ms)
{
    if (game == NULL) {
        return;
    }
    /* 主循环周期调用此函数；只比较时间并更新状态，不阻塞任务。 */
    if (game->active == GAME_ID_REACTION &&
        game->phase == GAME_PHASE_WAITING &&
        (int32_t)(now_ms - game->deadline_ms) >= 0) {
        game->phase = GAME_PHASE_READY;
        game->deadline_ms = now_ms;
    } else if (game->active == GAME_ID_SNAKE &&
               game->phase == GAME_PHASE_PLAYING &&
               (int32_t)(now_ms - game->next_step_ms) >= 0) {
        snake_step(game);
        game->next_step_ms = now_ms + SNAKE_STEP_MS;
    }
}
