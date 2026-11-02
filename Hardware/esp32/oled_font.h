#ifndef OLED_FONT_H
#define OLED_FONT_H

#include <stdbool.h>
#include <stdint.h>

/* 读取 5×7 ASCII 或 16×16 中文点阵；out 是调用方准备的输出数组。 */
bool oled_font_get_ascii(char value, uint8_t out[5]);
bool oled_font_get_chinese(uint16_t codepoint, uint8_t out[32]);

#endif
