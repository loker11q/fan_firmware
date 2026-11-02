#ifndef GAME_ENGINE_H
#define GAME_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

/* 贪吃蛇棋盘为 16×8 格；最多保存 128 个格子坐标。 */
#define GAME_SNAKE_CAPACITY 128U
#define GAME_GRID_WIDTH 16U
#define GAME_GRID_HEIGHT 8U

typedef enum {
    GAME_ID_SNAKE = 0, /* 贪吃蛇。 */
    GAME_ID_REACTION,  /* 反应速度测试。 */
    GAME_ID_GUESS,     /* 猜数字。 */
    GAME_ID_COUNT      /* 游戏种类数量，用作范围检查。 */
} game_id_t;

typedef enum {
    GAME_PHASE_MENU = 0, /* 处于游戏选择菜单。 */
    GAME_PHASE_PLAYING,  /* 正在操作游戏。 */
    GAME_PHASE_WAITING,  /* 反应测试随机等待。 */
    GAME_PHASE_READY,    /* 反应测试显示 GO，等待按键。 */
    GAME_PHASE_RESULT,   /* 已有结果，等待重试/确认。 */
    GAME_PHASE_PAUSED,   /* 贪吃蛇暂停。 */
    GAME_PHASE_OVER      /* 贪吃蛇结束。 */
} game_phase_t;

typedef enum {
    GAME_INPUT_LEFT = 0, /* 左转/数字减一。 */
    GAME_INPUT_RIGHT,    /* 右转/数字加一。 */
    GAME_INPUT_ACTION   /* 确认/暂停/继续/重试。 */
} game_input_t;

/* 游戏的全部可变状态集中放在这个结构体中，方便按时间片更新。
 * 坐标从左上角 (0,0) 开始；snake_x[i]/snake_y[i] 表示同一节身体。
 */
typedef struct {
    game_phase_t phase;             /* 当前流程阶段，如菜单、游戏中或结束。 */
    game_id_t selected;              /* 菜单里当前选中的游戏。 */
    game_id_t active;                /* 正在运行的游戏。 */
    uint32_t rng_state;               /* 伪随机数发生器的内部状态。 */
    uint32_t next_step_ms;            /* 贪吃蛇下次移动的系统时间。 */
    uint32_t deadline_ms;             /* 反应游戏等待结束的时间点。 */
    uint32_t reaction_ms;             /* 测得的反应时间，单位毫秒。 */
    uint8_t snake_x[GAME_SNAKE_CAPACITY]; /* 每一节身体的横坐标数组。 */
    uint8_t snake_y[GAME_SNAKE_CAPACITY]; /* 同下标对应上一行中的纵坐标。 */
    uint8_t snake_length;                  /* 当前蛇身实际使用的数组元素数。 */
    uint8_t snake_direction;               /* 内部方向编号：上/右/下/左。 */
    uint8_t food_x;                        /* 食物的横坐标。 */
    uint8_t food_y;                        /* 食物的纵坐标。 */
    uint8_t guess_low;                /* 猜数字当前允许范围的下限。 */
    uint8_t guess_high;               /* 猜数字当前允许范围的上限。 */
    uint8_t guess_value;              /* 用户当前选择的数字。 */
    uint8_t guess_target;              /* 程序生成的目标数字。 */
    int8_t guess_hint;                 /* 1=再大，-1=再小，0=暂无提示。 */
    bool false_start;                  /* 反应测试是否提前按键。 */
    bool won;                          /* 猜数字是否答对。 */
} game_engine_t;

/* seed 是随机数种子；相同种子会产生可重复的测试序列。 */
void game_engine_init(game_engine_t *game, uint32_t seed);
/* direction 大于 0 向后选，小于 0 向前选，并在菜单首尾循环。 */
void game_engine_menu_move(game_engine_t *game, int8_t direction);
/* 启动当前选中的游戏；now_ms 是当前毫秒计时值。 */
void game_engine_start(game_engine_t *game, uint32_t now_ms);
/* 将一次抽象按键事件交给当前游戏处理。 */
void game_engine_input(game_engine_t *game, game_input_t input, uint32_t now_ms);
/* 定期调用来推进游戏计时；不在这里延时等待。 */
void game_engine_tick(game_engine_t *game, uint32_t now_ms);

#endif
