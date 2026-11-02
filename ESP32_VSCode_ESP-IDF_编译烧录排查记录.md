# ESP32 在 VS Code 中使用 ESP-IDF 编译与烧录：排查记录

> 这篇文章记录一次 ESP32 风扇项目从安装 ESP-IDF、配置工程、排查串口，到成功烧录并显示 OLED 的过程。内容按实际日志和操作整理，区分已确认结论与仍待验证事项。

- **工程：** ESP32 BLDC 风扇固件，C 语言
- **开发环境：** Windows、VS Code、Espressif IDF 扩展、ESP-IDF v5.5.6
- **工程目录：** `G:\fan_firmware`
- **串口：** COM8，USB 转 UART
- **当前结果：** 固件已成功烧录，OLED 稳定显示
- **测试边界：** 电机未购买/未连接；按键、ADC、FG、Wi-Fi、HTTP、OTA 等其他功能尚未完成实物验证

## 目录

- [一、排查过程概览](#一排查过程概览)
- [二、ESP-IDF 安装慢与 Python 检查失败](#二esp-idf-安装慢与-python-检查失败)
- [三、menuconfig 自动检测串口失败](#三menuconfig-自动检测串口失败)
- [四、Set Target 清理构建文件后找不到 ELF](#四set-target-清理构建文件后找不到-elf)
- [五、USB 转串口缺少共地](#五usb-转串口缺少共地)
- [六、手动进入下载模式与烧录结果判断](#六手动进入下载模式与烧录结果判断)
- [七、烧录后启动崩溃并反复重启](#七烧录后启动崩溃并反复重启)
- [八、当前结果与未验证项目](#八当前结果与未验证项目)
- [九、经验总结](#九经验总结)
- [参考资料](#参考资料)

## 一、排查过程概览

这次并不是单一的“烧录失败”，而是几类问题先后出现：

1. ESP-IDF 安装进度很慢，Python 检查又提示缺少 `pip`。
2. SDK Configuration Editor（SDK 配置编辑器）自动探测 COM8 时，`chip_id` 命令收到无效串口数据。
3. `Set Target` 执行 `fullclean` 清理构建目录，之后没有先重新编译就点击 Flash，导致目标 `.elf` 不存在。
4. USB 转串口最初只连接 RX、TX，没有连接 GND；UART 没有共同的电压参考地。
5. 板子没有自动下载电路，需要用 KEY4/KEY5 手动控制启动模式。
6. 固件写入后，启动日志显示程序在 UART 控制台初始化时异常并自动重启。

排查时需要把三个阶段分开：

```text
本机编译产物是否存在
        ↓
电脑与 ESP32 是否能通过 UART 通信
        ↓
写入 Flash 后固件能否正常启动
```

`.elf` 缺失发生在第一阶段；COM8 无效数据发生在第二阶段；`Guru Meditation` 重启循环发生在第三阶段。一个阶段通过，不代表后续阶段也已经通过。

## 二、ESP-IDF 安装慢与 Python 检查失败

### 现象

- ESP-IDF Installation Manager（EIM）运行十几分钟后，进度仍只有约 3%。
- Python 环境检查要求 Python 版本在 `3.10` 到 `3.14` 之间，并提示 `pip` 未通过检查。
- 电脑上原本有多个 Python：3.6、3.9 和 3.11；`py -0p` 能列出这些版本，但 EIM 的检查结果仍不通过。
- 点击安装器中的“安装 Python”时，没有立即看到安装完成。

### 如何定位

先用 `py -0p` 查看电脑安装的 Python，再查看 EIM 日志，而不是把 Python 启动器列出的版本等同于 ESP-IDF 实际使用的 Python。

EIM 日志目录为：

```text
%LOCALAPPDATA%\eim\logs
```

日志显示，EIM 曾下载 Python 3.11.15 压缩包，但有一次解压失败：

```text
Failed to install python
Failed to extract Python archive
IO error: 系统找不到指定的文件 (os error 2)
```

之后的检查记录显示 Python 3.11.15 和 pip 检查通过；安装器也为 ESP-IDF 创建了独立虚拟环境：

```text
Python Version: Python 3.11.15
pip: ... (python 3.11)
Python environment installed successfully
Successfully installed IDF
```

ESP-IDF 专用 Python 环境路径是：

```text
G:\ESP-IDF\.espressif\v5.5.6\tools\python\v5.5.6\venv
```

### 原因与处理

电脑上虽然安装了 Python 3.11，但那不等于 EIM 已成功安装或识别它需要的 Python 和 `pip`。EIM 后来安装了自己的 Python 3.11.15，并为 IDF v5.5.6 建立专用虚拟环境。工具链和组件随后继续下载并完成安装。

查看系统 Python 时使用的是：

```powershell
py -0p
```

当时结果包含系统 Python 3.9，以及 Anaconda 环境中的 Python 3.6 和 3.11。安装器后续日志则确认它使用自己的 Python 3.11.15 和 IDF 专用 `venv`。所以 `py -0p` 用来盘点电脑上的 Python，不代表 ESP-IDF 最终会使用其中某个 Anaconda 环境。

从 EIM 日志看，Python 下载/配置、工具链下载解压、组件同步是分开的步骤。进度长时间不动时，日志中的下载和解压记录比界面百分比更能说明安装器正在做什么。

## 三、menuconfig 自动检测串口失败

### 现象

执行 `idf.py menuconfig` 时，SDK Configuration Editor 尝试自动识别开发板，并通过 COM8 执行芯片识别命令。串口被找到，但 `chip_id` 检测失败：

```text
Found 1 serial ports
Serial port COM8
Connecting................
COM8 failed to connect: Invalid head of packet (0x65)
A fatal error occurred: Could not connect to an Espressif device
```

### 如何区分问题范围

`idf.py menuconfig` 是 ESP-IDF 自带的项目配置命令，不是用户编写的 Python 脚本。这里失败的是扩展对板子进行的**串口自动探测**；它不等于菜单配置文件损坏，也不等于编译已经失败。

ESP32 没进入下载模式、UART 接线不正确、串口没有共同地，都可能让电脑收到不符合 esptool 协议的数据。因此看到 `Invalid head of packet` 时，不能只在软件菜单里找原因，还要检查串口物理连接和启动模式。

后续检查发现转接器当时没有和板子共地（见下一节）。因此这次芯片探测失败应先按串口链路问题排查，而不是据此判断 menuconfig 或工程配置损坏。

## 四、Set Target 清理构建文件后找不到 ELF

### 现象

执行 `Set Target` 后，日志明确显示它先运行了 `fullclean`，并把原有 `sdkconfig` 改名：

```text
Adding "set-target"'s dependency "fullclean"
Executing action: fullclean
Existing sdkconfig ... renamed to ... sdkconfig.old
Set Target to: esp32, new sdkconfig will be created.
```

紧接着的 Flash 输出是：

```text
The file g:\fan_firmware\build\esp32_bldc_fan.elf could not be found.
Flash task failed.
```

这次 `Set Target` 之后的输出完成了 CMake 配置，但在 Flash 报错前没有看到应用编译步骤，也没有看到 `Writing at...` 或 `Hash of data verified`。

### 原因

`fullclean` 清理的是电脑工程中的构建产物。旧 `.elf` 和 `.bin` 被清除后，`Set Target` 重新配置了工程；随后 Flash 任务要读取 `build\esp32_bldc_fan.elf`，但此时文件尚未重新生成。

这不是 KEY4/KEY5 的问题。按键能让 ESP32 进入下载模式，但不会在电脑上生成 `.elf`。

### 处理

目标芯片已经是 ESP32 时，不要为了每次烧录重复执行 `Set Target`。如果确实更改了目标芯片，`Set Target` 后先执行一次完整构建并等待成功，再执行 Flash：

```powershell
idf.py build
```

VS Code 中对应的操作是先运行 **Build**，确认工程构建完成、`build\esp32_bldc_fan.elf` 已生成，再运行 **Flash**。若 Build 本身报错，Flash 阶段缺少 ELF 是后续结果，不是独立的串口故障。

### 为什么 `Flash Done` 不能作为成功证据

同一段扩展输出在 `Flash task failed` 后又出现了 `Flash Done`。这类汇总提示不能覆盖前面的明确错误。判断烧录是否真正开始，应看 esptool 是否识别芯片，并进一步看到数据写入和校验，例如：

```text
Chip is ESP32-D0WD-V3 (revision v3.1)
Wrote ... at 0x...
Hash of data verified.
```

如果停在“找不到 `.elf`”，任务尚未进入 UART 芯片通信和写入阶段；这时也不能判断下载模式是否正常。

## 五、USB 转串口缺少共地

### 现象与发现

早期 USB 转串口模块连接到板子时只接了 RX、TX，没有连接 GND。之后复盘确认，这是 COM8 收到无效数据、无法稳定通信的重要硬件原因。

UART 的高低电平是相对于 GND 测量的。电脑侧和 ESP32 侧没有共同参考地时，接收端无法可靠判断对方传来的电压是逻辑 0 还是逻辑 1；TX/RX 即使接对，也可能出现乱码或握手失败。

### V2.4 UART 接线

按 V2.4 测试基线，USB 转串口和板子之间的连接为：

| USB 转串口模块 | 风扇板 H6 / ESP32 | 说明 |
|---|---|---|
| TXD | H6-3 / GPIO3 / U0RXD | 转接器发送，接 ESP32 接收 |
| RXD | H6-2 / GPIO1 / U0TXD | 转接器接收，接 ESP32 发送 |
| GND | H6-4 / GND | 两侧必须共地 |
| VCC | 不连接 | 不让转接器给板子并联供电 |

这块板的 Type-C 不提供 USB 数据烧录，也没有 DTR/RTS 自动下载电路。串口线序、共地和供电方式是三项独立检查点；不能用“COM8 出现了”代替它们。

## 六、手动进入下载模式与烧录结果判断

### KEY4/KEY5 操作顺序

KEY4 是 BOOT（GPIO0），KEY5 是 RESET（EN）。本板需要手动进入 ROM 下载模式：

1. 按住 KEY4。
2. 按下再松开 KEY5，期间保持 KEY4 按下。
3. 松开 KEY4。
4. 启动 Flash 操作。
5. 烧录完成后，松开 KEY4 状态下按下再松开 KEY5，让芯片正常启动 Flash 中的固件。

ESP32 在复位时读取 GPIO0 的状态：GPIO0 被拉低会进入 UART 下载模式，否则正常从 Flash 启动。[Espressif Boot Mode 说明](https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/boot-mode-selection.html)

进入 ROM 下载模式解决的是“电脑能否和芯片下载程序通信”；它与电脑上是否存在 `.elf` 是不同的问题。下载模式正常但构建目录没有 `.elf`，Flash 仍无法开始。

### 识别真正的写入成功

后续成功的 esptool 输出中，芯片被识别为 ESP32-D0WD-V3，随后多个镜像区域写入并通过哈希校验：

```text
Chip is ESP32-D0WD-V3 (revision v3.1)
Uploading stub...
Wrote ... at 0x00001000 ...
Hash of data verified.
Wrote ... at 0x00020000 ...
Hash of data verified.
Wrote ... at 0x00008000 ...
Hash of data verified.
```

这些记录表明 esptool 已连上芯片并完成对应区域的写入校验。与只出现 `Flash Done` 相比，`Wrote` 和 `Hash of data verified` 是更直接的写入证据。[ESP-IDF 构建与烧录流程](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/get-started/windows-setup.html)

### `Hard resetting via RTS pin` 的含义

烧录结束时出现：

```text
Leaving...
Hard resetting via RTS pin...
```

这是 esptool 尝试通过串口控制线 RTS 复位芯片。当前板没有把 USB 转串口的 DTR/RTS 接到 GPIO0/EN 的自动下载电路，所以这行输出不能保证板子真的已被 RTS 复位。若前面已经有写入和哈希校验记录，烧录数据本身可以成功；之后仍需用 KEY5 手动复位运行新固件。[自动与手动下载模式说明](https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/boot-mode-selection.html)

## 七、烧录后启动崩溃并反复重启

### 现象与定位

一次启动日志先报告 OLED 被找到、Wi-Fi SSID 未配置，随后出现 `Guru Meditation` 和 `LoadProhibited`；异常处理后不断重启：

```text
OLED found at 0x3C (128 x 64)
Wi-Fi SSID is not configured; network API is disabled
Guru Meditation Error: Core 0 panic'ed (LoadProhibited)
Panic handler entered multiple times. Abort panic handling. Rebooting ...
```

回溯解析定位到：

```text
network_port_register_ota_command at Hardware/esp32/network_ota.c
app_main at main/app_main.c
```

这说明 Flash 写入已经不是当时的主要问题；芯片在运行新程序时，应用启动代码触发了异常。Wi-Fi 未配置的日志与崩溃相邻出现，但它本身是网络功能未配置提示，不是回溯指向的崩溃点。

### 软件原因

原代码先手动调用 `esp_console_init()` 初始化控制台，之后再调用 `esp_console_new_repl_uart()` 创建 UART REPL（交互式命令行）。ESP-IDF 的 REPL 初始化流程也会初始化控制台；因此原来的调用顺序造成重复初始化。

失败路径中，UART VFS（虚拟文件系统）已经切换成经 UART 驱动输出，初始化清理又删除了驱动。随后错误处理调用 `ESP_LOGE` 打印错误，日志仍经由同一 UART VFS 输出，形成递归报错。回溯中反复出现 UART 写入、日志和 VFS 输出函数，与这一过程相符。ESP-IDF 文档也提醒，VFS 写入失败时再经同一路径记录日志可能形成递归。[Console API](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32/api-reference/system/console.html) · [Standard I/O 与 Console Output](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32/api-guides/stdio.html)

### 代码处理与验证

在 `Hardware/esp32/network_ota.c` 中调整初始化顺序：

1. 先创建 UART REPL，让 ESP-IDF 完成控制台初始化。
2. 再注册 `ota` 命令。
3. 最后启动 REPL。
4. 如果 REPL 创建失败，先把 UART VFS 恢复为直接、非阻塞输出，再打印错误，避免错误日志进入已删除的 UART 驱动。

修改后工程编译通过并生成了 `.elf` 和 `.bin`。用户随后确认固件已烧录，OLED 可稳定显示。此验证说明当前固件能启动并驱动 OLED；不代表风扇和所有外设均已验证。

## 八、当前结果与未验证项目

| 项目 | 当前状态 | 说明 |
|---|---|---|
| ESP-IDF v5.5.6 安装 | 已完成 | EIM 日志记录 IDF 安装成功 |
| 工程构建产物 | 已生成 | `build/esp32_bldc_fan.elf` 与 `.bin` 已存在 |
| UART 写入与校验 | 已完成 | 实际日志出现芯片识别、`Wrote` 和 `Hash of data verified` |
| OLED | 已确认 | 当前 OLED 稳定显示 |
| 电机/PWM | 未验证 | 电机未购买/未接入；PWM 保持安全配置 |
| 按键、ADC、电池、FG | 未验证 | 需要后续按硬件基线逐项测试 |
| Wi-Fi、HTTP、OTA、回滚 | 未验证 | Wi-Fi 凭据未配置，不能声称这些功能已实测通过 |

### 仍待单独核查的启动警告

旧日志还包含：

```text
gpio_pullup_en(85): GPIO number error (input-only pad has no internal PU)
```

当前工程源码把 FG 输入配置为 GPIO35，而日志里的数字是 `85`，两者不一致。V2.4 基线记录 GPIO35 的 FG 信号有外部 10 kΩ 上拉，但这还不能解释旧日志中的 `85`。因此把这条警告保留为待核查项，不将它说成已修复，也不把它和 UART 控制台崩溃混为一谈。

日志中的 I²C 上拉提示也应与“屏幕能显示”分开看：OLED 当前可显示，但模块上拉电阻和总线电气状态仍应按硬件实物确认。Wi-Fi SSID 未配置提示表示网络 API 被禁用；它不等同于硬件烧录错误。

## 九、经验总结

1. **先确认构建产物，再处理烧录连接。** `.elf` 缺失是本机工程构建目录问题；COM8 握手失败是串口/下载模式问题。
2. **UART 至少需要 TX、RX 和共同 GND。** TX/RX 交叉连接，GND 共地；转接器 VCC 不接板子。
3. **本板没有自动下载电路。** KEY4/KEY5 的手动时序负责控制 GPIO0 与复位，不负责编译或创建 `.elf`。
4. **按完整证据判断烧录。** `Chip is...`、`Wrote...`、`Hash of data verified` 才能说明进入芯片通信和数据写入阶段；汇总提示不能覆盖明确失败。
5. **区分烧录成功与固件功能验证。** OLED 稳定显示证明固件已运行到显示部分；没有接电机、未测 ADC/FG、未配置 Wi-Fi 时，不能说整机功能已完成验证。

## 参考资料

- [项目架构与上手操作指南](./项目架构与上手操作指南.md)
- [ESP-IDF v5.5 Windows 构建与烧录说明](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/get-started/windows-setup.html)
- [ESP-IDF v5.5.6 Console API](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32/api-reference/system/console.html)
- [ESP-IDF v5.5.6 Standard I/O 与 Console Output](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32/api-guides/stdio.html)
- [Espressif esptool：ESP32 启动模式与手动下载模式](https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/boot-mode-selection.html)
- 板级 GPIO、UART 与电源信息依据 ESP32 BLDC Fan V2.4 电池供电接线与固件测试基线；首次烧录前应以实际板卡和测试记录为准。
