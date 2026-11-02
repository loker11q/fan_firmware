#ifndef SSD1306_H
#define SSD1306_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"

/* 将 SSD1306 接到已创建的总线上；address 为 7 位地址，宽高单位为像素。 */
int ssd1306_attach(i2c_master_bus_handle_t bus,
                   uint8_t address,
                   uint8_t width,
                   uint8_t height);
/* 下面的绘图函数先改本机 framebuffer（显存缓冲区），flush 才通过 I²C 发给屏幕。
 * 坐标原点在左上角；越过屏幕边界的像素会被忽略。
 */
void ssd1306_clear(void);
void ssd1306_draw_text(uint8_t x, uint8_t y, const char *utf8_text);
void ssd1306_draw_pixel(uint8_t x, uint8_t y, bool on);
void ssd1306_fill_rect(uint8_t x, uint8_t y, uint8_t width, uint8_t height);
int ssd1306_flush(void);

#endif
