#include "ssd1306.h"
#include "oled_font.h"

#include <stddef.h>
#include <string.h>

#include "esp_err.h"

#define OLED_MAX_WIDTH 128U
#define OLED_MAX_HEIGHT 64U
#define OLED_FRAMEBUFFER_SIZE (OLED_MAX_WIDTH * OLED_MAX_HEIGHT / 8U)
#define OLED_TIMEOUT_MS 100

/* s_framebuffer 是软件显存：一个字节对应竖直方向 8 个像素。
 * 每次绘图先改 RAM，ssd1306_flush() 再把它发送到屏幕。
 */
static i2c_master_dev_handle_t s_device;
static uint8_t s_width;
static uint8_t s_height;
static uint8_t s_framebuffer[OLED_FRAMEBUFFER_SIZE];

static int send_command(uint8_t command)
{
    /* SSD1306 控制字节 0x00 表示后一个字节是命令。 */
    const uint8_t bytes[2] = { 0x00U, command };
    return i2c_master_transmit(s_device, bytes, sizeof(bytes), OLED_TIMEOUT_MS) == ESP_OK ?
           0 : -1;
}

static int send_data(const uint8_t *data, size_t length)
{
    /* 0x40 表示后续内容是显示数据；长度受单页宽度限制。 */
    uint8_t packet[OLED_MAX_WIDTH + 1U];
    if (data == NULL || length > OLED_MAX_WIDTH) {
        return -1;
    }
    packet[0] = 0x40U;
    memcpy(&packet[1], data, length);
    return i2c_master_transmit(s_device, packet, length + 1U, OLED_TIMEOUT_MS) == ESP_OK ?
           0 : -1;
}

static uint32_t utf8_next(const unsigned char **cursor)
{
    /* UTF-8 字符长度为 1～4 字节；cursor 是指向“当前读取位置”的指针地址。
     * 解析后移动 *cursor，使下一次循环从下一个字符开始。
     */
    const unsigned char *p = *cursor;
    uint32_t codepoint;
    if (p[0] < 0x80U) {
        *cursor = p + 1;
        return p[0];
    }
    if ((p[0] & 0xE0U) == 0xC0U && p[1] != 0U) {
        codepoint = ((uint32_t)(p[0] & 0x1FU) << 6) |
                    (uint32_t)(p[1] & 0x3FU);
        *cursor = p + 2;
        return codepoint;
    }
    if ((p[0] & 0xF0U) == 0xE0U && p[1] != 0U && p[2] != 0U) {
        codepoint = ((uint32_t)(p[0] & 0x0FU) << 12) |
                    ((uint32_t)(p[1] & 0x3FU) << 6) |
                    (uint32_t)(p[2] & 0x3FU);
        *cursor = p + 3;
        return codepoint;
    }
    if ((p[0] & 0xF8U) == 0xF0U && p[1] != 0U && p[2] != 0U && p[3] != 0U) {
        *cursor = p + 4;
        return (uint32_t)'?';
    }
    *cursor = p + 1;
    return (uint32_t)'?';
}

int ssd1306_attach(i2c_master_bus_handle_t bus,
                   uint8_t address,
                   uint8_t width,
                   uint8_t height)
{
    /* 创建设备前先检查缓冲区能否容纳分辨率，再探测 I²C 地址。 */
    if (bus == NULL || width == 0U || width > OLED_MAX_WIDTH ||
        height == 0U || height > OLED_MAX_HEIGHT || (height % 8U) != 0U) {
        return -1;
    }
    if (i2c_master_probe(bus, address, OLED_TIMEOUT_MS) != ESP_OK) {
        return -1;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 100000U,
        .flags.disable_ack_check = false,
    };
    if (i2c_master_bus_add_device(bus, &device_config, &s_device) != ESP_OK) {
        s_device = NULL;
        return -1;
    }

    s_width = width;
    s_height = height;
    /* SSD1306 上电初始化命令序列；参数需与屏幕高度/接线方向匹配。 */
    const uint8_t init_commands[] = {
        0xAEU, 0xD5U, 0x80U, 0xA8U, (uint8_t)(height - 1U),
        0xD3U, 0x00U, 0x40U, 0x8DU, 0x14U, 0x20U, 0x02U,
        0xA1U, 0xC8U, 0xDAU, (uint8_t)(height == 64U ? 0x12U : 0x02U),
        0x81U, 0xCFU, 0xD9U, 0xF1U, 0xDBU, 0x40U, 0xA4U, 0xA6U, 0xAFU
    };
    for (size_t i = 0U; i < sizeof(init_commands); ++i) {
        if (send_command(init_commands[i]) != 0) {
            (void)i2c_master_bus_rm_device(s_device);
            s_device = NULL;
            return -1;
        }
    }
    ssd1306_clear();
    if (ssd1306_flush() != 0) {
        (void)i2c_master_bus_rm_device(s_device);
        s_device = NULL;
        return -1;
    }
    return 0;
}

void ssd1306_clear(void)
{
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
}

void ssd1306_draw_pixel(uint8_t x, uint8_t y, bool on)
{
    if (x >= s_width || y >= s_height) {
        return;
    }
    /* y/8 找到竖向页，y%8 找到该字节中的某一位。 */
    const size_t index = (size_t)(y / 8U) * OLED_MAX_WIDTH + x;
    /* 1U << 位号得到单像素掩码；|= 点亮，&= ~mask 熄灭，其余像素不变。 */
    const uint8_t mask = (uint8_t)(1U << (y & 7U));
    if (on) {
        s_framebuffer[index] |= mask;
    } else {
        s_framebuffer[index] &= (uint8_t)~mask;
    }
}

void ssd1306_fill_rect(uint8_t x, uint8_t y, uint8_t width, uint8_t height)
{
    /* 使用 16 位临时坐标做加法，避免 uint8_t 的 x+width 溢出回绕。 */
    for (uint16_t row = y; row < (uint16_t)y + height; ++row) {
        for (uint16_t col = x; col < (uint16_t)x + width; ++col) {
            if (row < s_height && col < s_width) {
                ssd1306_draw_pixel((uint8_t)col, (uint8_t)row, true);
            }
        }
    }
}

static void draw_ascii(uint8_t x, uint8_t y, char value)
{
    /* ASCII 字模按列存储；这里统一转大写，当前字库不区分大小写。 */
    uint8_t columns[5];
    char normalized = value;
    if (normalized >= 'a' && normalized <= 'z') {
        normalized = (char)(normalized - 'a' + 'A');
    }
    if (!oled_font_get_ascii(normalized, columns)) {
        (void)oled_font_get_ascii('?', columns);
    }
    for (uint8_t col = 0U; col < 5U; ++col) {
        for (uint8_t row = 0U; row < 7U; ++row) {
            if ((columns[col] & (uint8_t)(1U << row)) != 0U) {
                ssd1306_draw_pixel((uint8_t)(x + col), (uint8_t)(y + row), true);
            }
        }
    }
}

static void draw_chinese(uint8_t x, uint8_t y, uint16_t codepoint)
{
    /* 每个中文字 16×16=256 个点，占 32 字节，按行取出每行两字节。 */
    uint8_t bitmap[32];
    if (!oled_font_get_chinese(codepoint, bitmap)) {
        draw_ascii(x, y, '?');
        return;
    }
    for (uint8_t row = 0U; row < 16U; ++row) {
        for (uint8_t col = 0U; col < 16U; ++col) {
            const uint8_t byte = bitmap[(size_t)row * 2U + (col / 8U)];
            if ((byte & (uint8_t)(1U << (7U - (col & 7U)))) != 0U) {
                ssd1306_draw_pixel((uint8_t)(x + col), (uint8_t)(y + row), true);
            }
        }
    }
}

void ssd1306_draw_text(uint8_t x, uint8_t y, const char *utf8_text)
{
    if (utf8_text == NULL) {
        return;
    }
    /* C 字符串以 0 结尾；UTF-8 中文不能按 char 一个字节一个字节直接画。 */
    const unsigned char *cursor = (const unsigned char *)utf8_text;
    while (*cursor != 0U && x < s_width && y < s_height) {
        const uint32_t codepoint = utf8_next(&cursor);
        if (codepoint == '\n') {
            x = 0U;
            y = (uint8_t)(y + 16U);
        } else if (codepoint < 0x80U) {
            draw_ascii(x, y, (char)codepoint);
            x = (uint8_t)(x + 6U);
        } else if (codepoint <= 0xFFFFU) {
            draw_chinese(x, y, (uint16_t)codepoint);
            x = (uint8_t)(x + 16U);
        } else {
            draw_ascii(x, y, '?');
            x = (uint8_t)(x + 6U);
        }
    }
}

int ssd1306_flush(void)
{
    if (s_device == NULL) {
        return -1;
    }
    /* 按屏幕页逐块发送，避免一次 I²C 缓冲区过大。 */
    const uint8_t page_count = (uint8_t)(s_height / 8U);
    for (uint8_t page = 0U; page < page_count; ++page) {
        if (send_command((uint8_t)(0xB0U | page)) != 0 ||
            send_command(0x00U) != 0 || send_command(0x10U) != 0) {
            return -1;
        }
        const uint8_t *page_data = &s_framebuffer[(size_t)page * OLED_MAX_WIDTH];
        if (send_data(page_data, s_width) != 0) {
            return -1;
        }
    }
    return 0;
}
