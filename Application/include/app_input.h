#ifndef APP_INPUT_H
#define APP_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* app_input_update 一次最多产生的事件数；调用方要准备同样长度的数组。 */
#define APP_INPUT_MAX_EVENTS 4U

/* 按键状态机所处的界面。切换模式后会清理未完成的组合按键判断。 */
typedef enum {
    APP_INPUT_MODE_MAIN = 0, /* 风扇主页面。 */
    APP_INPUT_MODE_GAME_MENU,/* 选择小游戏。 */
    APP_INPUT_MODE_GAME      /* 正在玩小游戏。 */
} app_input_mode_t;

/* 输入层只报告“发生了什么”，不直接控制风扇或 OLED。 */
typedef enum {
    APP_EVENT_NONE = 0,
    APP_EVENT_SPEED_UP,       /* KEY1 加档。 */
    APP_EVENT_SPEED_DOWN,     /* KEY2 减档。 */
    APP_EVENT_PAGE_TOGGLE,    /* KEY1+KEY2 短按切换显示页。 */
    APP_EVENT_GAME_ENTER,     /* KEY1+KEY2 长按进入小游戏菜单。 */
    APP_EVENT_FAN_TOGGLE,     /* KEY3 短按切换风扇软件运行状态。 */
    APP_EVENT_GAME_MENU_PREV, /* 游戏菜单选上一个。 */
    APP_EVENT_GAME_MENU_NEXT, /* 游戏菜单选下一个。 */
    APP_EVENT_GAME_SELECT,    /* KEY3 确认启动菜单选中的游戏。 */
    APP_EVENT_GAME_LEFT,      /* 游戏内左方向/减小数字。 */
    APP_EVENT_GAME_RIGHT,     /* 游戏内右方向/增加数字。 */
    APP_EVENT_GAME_ACTION,    /* 游戏内确认/暂停/重试。 */
    APP_EVENT_GAME_BACK,      /* 从游戏回到游戏菜单。 */
    APP_EVENT_GAME_EXIT       /* 从游戏菜单回主界面。 */
} app_event_type_t;

typedef struct {
    app_event_type_t type;      /* 事件类型，例如加档或切换页面。 */
    uint32_t timestamp_ms;      /* 事件发生时间，供游戏计时使用。 */
} app_event_t;

typedef struct {
    bool raw_pressed;                 /* 最近一次读到的电平换算结果。 */
    bool stable_pressed;              /* 消抖后确认的按下状态。 */
    bool suppress_until_release;      /* 切换界面时避免旧按键误触新界面。 */
    uint32_t raw_changed_ms;           /* 原始状态上次变化的时间。 */
    uint32_t pressed_ms;               /* 本次稳定按下开始的时间。 */
} app_button_state_t;

typedef struct {
    app_input_mode_t mode;              /* 主界面、游戏菜单或游戏中。 */
    app_button_state_t button[3];       /* 只处理 KEY1、KEY2、KEY3。 */
    int8_t pending_speed_key;           /* 等待 200 ms，以判断是否为组合键；-1 表示无。 */
    uint32_t pending_speed_ms;          /* 第一个速度键稳定按下的时间。 */
    bool pending_speed_emitted;         /* 防止同一次按住重复加/减档。 */
    bool combo_active;                  /* KEY1 与 KEY2 已组成组合键。 */
    bool combo_enter_emitted;           /* 长按进入游戏事件是否已经发送。 */
    uint32_t combo_started_ms;          /* 两键组合按下的起始时间。 */
    bool key3_long_emitted;             /* 防止 KEY3 长按事件重复发送。 */
} app_input_t;

/* 清零状态并进入主界面。传入的是调用方拥有的状态结构体地址。 */
void app_input_init(app_input_t *input);
/* 改变按键解释方式，例如从主界面切换到游戏菜单。 */
void app_input_set_mode(app_input_t *input, app_input_mode_t mode);
/* pressed_button_mask 的 bit0/1/2 分别代表 KEY1/2/3，1 表示按下。
 * events 是调用方提供的输出数组；返回值是本次写入的事件数量。
 */
size_t app_input_update(app_input_t *input,
                        uint8_t pressed_button_mask,
                        uint32_t now_ms,
                        app_event_t out_events[APP_INPUT_MAX_EVENTS]);

#endif
