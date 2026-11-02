# ESP32 BLDC 风扇固件

本工程以 ESP32-WROOM-32E-H4、经典 ESP32 和 ESP-IDF v5.5.6 为目标。代码使用 C 编写，电机参数未确认时，风扇 PWM 默认关闭；电机未接入时仍可验证按键、OLED、ADC 读数框架、小游戏和局域网状态接口。

## 目录分层

| 目录 | 作用 | 是否依赖 ESP-IDF |
|---|---|---|
| `Application/` | 风扇档位、按键状态机、贪吃蛇/反应测试/猜数字、电池换算等纯 C 逻辑 | 否 |
| `Services/` | 把业务状态应用到硬件接口，组织 OLED 页面 | 通过接口间接使用硬件 |
| `Hardware/include/` | 硬件与网络端口接口；定义上层可调用的函数 | 否 |
| `Hardware/esp32/` | GPIO、LEDC PWM、ADC、PCNT FG、I²C OLED、Wi-Fi、HTTP 和 OTA 的 ESP32 实现 | 是 |
| `main/` | ESP-IDF 启动、FreeRTOS 任务和模块装配 | 是 |

STM32 移植时，`Application/` 的核心逻辑不应依赖 ESP32 寄存器或 ESP-IDF 类型。需要新增 STM32 的 `Hardware` 实现，并适配 `main` 启动入口、RTOS/时基、构建配置和 Flash 分区/引导程序；Wi-Fi、HTTP、HTTPS OTA 也需要目标平台提供等价服务。因此移植主要隔离在平台适配部分，但不是只改一个 GPIO 文件。

## 已实现功能

- KEY1 加档、KEY2 减档，10 档边界保护；KEY3 短按切换风扇运行状态。
- KEY1+KEY2 在 200 ms 组合窗口内短按切换 OLED 页面，长按 1 秒进入游戏菜单。风扇运行时拒绝进入游戏并显示提示。
- SSD1306 目标驱动通过 I²C 探测 `0x3C`、`0x3D`；缺少 OLED 不会阻止风扇应用启动。实际 OLED 控制器、引脚顺序、地址和上拉仍需按模块实物确认。
- 风扇页显示状态、档位、PWM、FG 转速、电池电压；系统页显示 Wi-Fi、IP、固件版本、OTA 状态和错误。
- 小游戏：贪吃蛇、反应速度测试、猜数字。游戏逻辑由状态机按时间片运行，不调用阻塞延时。
- Wi-Fi STA 连接后提供只读 `GET /api/v1/status`。接口不提供控制命令，也没有身份认证，仅建议用于可信局域网。
- UART 控制台命令 `ota` 从配置的 HTTPS 地址下载固件；校验服务器证书并依赖系统时间同步。启用官方 OTA 分区和首次启动自检/失败回滚。

### 按键操作

| 状态 | KEY1 | KEY2 | KEY3 |
|---|---|---|---|
| 风扇主界面 | 加一档 | 减一档 | 短按启动/停止 |
| 两键组合 | 与 KEY2 在 200 ms 内同时按下后短按，切换 OLED 页面；长按 1 秒进入游戏菜单 | 同左 | — |
| 游戏菜单 | 上一个游戏 | 下一个游戏 | 短按开始；长按退出游戏模式 |
| 贪吃蛇 | 左转 | 右转 | 短按暂停/继续；长按回游戏菜单 |
| 反应测试 | 提前按键会判定抢按；亮起 GO 后按任一功能键记录反应时间 | 同左 | 短按重试；长按回游戏菜单 |
| 猜数字 | 数值减一 | 数值加一 | 短按确认/提交；长按回游戏菜单 |

进入小游戏前必须先停止风扇。KEY4 仍为 BOOT，KEY5 仍为 RESET。

## 安全默认值

以下参数必须依据实际电机、电池和测量结果确认。首次上电保持默认值，不接电机：

| 配置项 | 默认值 | 作用 |
|---|---:|---|
| `FAN_MOTOR_ENABLE` | 关闭 | 禁止产生电机 PWM |
| `FAN_PWM_FREQ_HZ` | 0 | PWM 频率未确认 |
| `FAN_PWM_MAX_DUTY_PERCENT` | 0 | 最大占空比未确认 |
| `FAN_PWM_ACTIVE_LOW` | 否 | PWM 极性未确认 |
| `FAN_FG_PULSES_PER_REV` | 0 | FG 每转脉冲数未确认，因此不显示 RPM |
| 电池告警/停机阈值 | 0 mV | 未确认前不做电压百分比换算或低压停机 |

电池 ADC 只有在 ESP32 ADC 校准可用时才报告有效值。`battery_mv` 是总电压估算，不代表准确剩余电量，也不能判断两节电芯是否失衡。若配置了低压停机阈值而 ADC 读取失败，固件会停止风扇输出。

## VS Code 与 ESP-IDF 构建

1. 安装 VS Code 扩展 `Espressif IDF`（扩展标识：`espressif.esp-idf-extension`），并在扩展设置中安装/选择 ESP-IDF v5.5.6 及工具链。
2. 打开本目录 `G:\风扇\风扇测试`，在 ESP-IDF 终端执行：

   ```powershell
   idf.py set-target esp32
   idf.py menuconfig
   idf.py build
   ```

3. 在 `Fan application configuration` 菜单填写本机 Wi-Fi SSID、密码和 HTTPS 固件地址。`sdkconfig` 已加入忽略列表，个人凭据不会提交到工程文件。
4. 确认 `partitions.csv` 仅用于已确认的 4 MB Flash 模组。分区表包括两个各 `0x1F0000` 字节的 OTA 应用槽，单个固件必须能放入该槽。

当前工作环境中检测到 GCC，但没有 `idf.py`、CMake 和 Ninja，因此本次无法执行 ESP-IDF 编译、链接或生成可烧录镜像。主机侧纯 C 核心测试可单独执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\tests\host\run.ps1
```

## H6 串口烧录与上电检查

V2.4 测试基线规定 H6 是 3.3 V UART，Type-C 只供电/充电，不传输固件数据；板上没有 USB 自动下载电路。USB 转串口连接：

```text
转串口 TXD -> H6-3 / ESP32 GPIO3 / U0RXD
转串口 RXD -> H6-2 / ESP32 GPIO1 / U0TXD
转串口 GND -> H6-4 / GND
转串口 VCC -> 不连接；不要与 H6-1 V33 并联供电
```

进入下载模式时按住 KEY4（GPIO0/BOOT），按下并松开 KEY5（EN/RESET），然后松开 KEY4。烧录命令连接时需要手动完成按键时序；具体 `idf.py flash` 与串口工具行为要用本机 ESP-IDF 版本确认。烧录后松开 KEY4 并复位运行。

在烧录自定义分区表前，先确认模组实物型号、Flash 容量及 H6 芯片通信。当前硬件基线记录的 `read_mac`/`flash_id` 尚未成功，因此当前不能把芯片识别、串口链路或分区写入视为已验证。

电池接线和断电检查遵循同目录外部基线文件：`G:\风扇\codex修改\ESP32_BLDC_Fan_V2.4_电池供电接线与固件测试基线.md`。按该基线确认临时飞线已拆除、R9 状态已恢复、保护板端子/极性正确，再接 2S 电池。初次功能测试不接电机；用示波器验证 GPIO4 PWM，用外部信号验证 GPIO35 FG 计数。KEY4 和 KEY5 保持 BOOT/RESET 用途。

## OTA 与回滚

在 `menuconfig` 配置 `FAN_OTA_URL` 为 HTTPS 直链应用镜像。设备联网且系统时间同步后，在 H6 UART 控制台输入：

```text
ota
```

执行 OTA 前需要先停止风扇；OTA 开始后固件会临时禁止重新启动风扇。

失败时不会切换当前启动分区；成功写入新槽后设备重启。首次启动自检包含硬件初始化、按键读取、队列创建和 PWM 零输出检查；自检通过后调用 ESP-IDF API 确认新镜像，自检失败则请求官方引导程序回滚。

`FAN_SELFTEST_FORCE_FAIL` 仅用于 OTA 回滚实验：需先有一个可启动的旧固件槽，再升级到启用该选项的新镜像。不要在没有确认旧固件可启动时做该测试。

## OLED 字模

`Hardware/esp32/oled_font.c` 只包含工程运行时用到的中文字符和 ASCII 字模。字模基于 Noto Sans CJK SC 栅格化，许可证见 `Hardware/esp32/OFL-Noto-Sans-CJK.txt`。如 UI 增加新的中文字符串，可用安装了 Pillow 的 Python 运行：

```powershell
python .\tools\generate_oled_font.py C:\path\to\NotoSansCJKsc-Regular.otf
```

该命令会扫描 `ui_renderer.c` 和 `app_main.c` 中的字符串并重建字库；字体文件本身不放入仓库。推荐字体来源为 [Noto Sans CJK SC 官方仓库](https://github.com/notofonts/noto-cjk/blob/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf)，生成的 C 文件会记录输入字体 SHA-256。
