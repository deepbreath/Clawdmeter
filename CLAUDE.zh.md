# CLAUDE.zh.md

本文件为 Claude Code（claude.ai/code）在此仓库中工作时提供指引。英文版见 CLAUDE.md。

# 项目背景

为 **Waveshare ESP32-S3-Touch-AMOLED-2.16**（480×480 方形 AMOLED）设计的 ESP32-S3 固件，用于桌面旁的 Claude Code 用量监控器。通过 BLE 连接宿主机守护进程，守护进程轮询 Anthropic API 获取用量数据。

## 硬件（关键引脚）

- 显示屏：**CO5300** AMOLED，QSPI 接口（CS=12, SCLK=38, SDIO0..3=4..7, RST=2）
- 触摸：**CST9220**，I2C 接口（SDA=15, SCL=14, INT=11, 地址=0x5A）
- 电源管理：**AXP2101**，同一 I2C 总线（地址=0x34）— 电池、USB VBUS、电源键中断
- IMU：**QMI8658**，同一 I2C 总线（地址=0x6B）— 加速度计，用于自动旋转
- 按键：GPIO 0（左键 → Space/语音模式）、GPIO 18（右键 → Shift+Tab/模式切换）、AXP PKEY（中键 → 循环切换屏幕；在动画屏时 → 循环切换动画）

## 架构

```text
main.cpp           — setup()、loop()、按键轮询（左→Space，右→Shift+Tab，中→循环）、旋转闪屏
display_cfg.h      — 引脚定义，extern 对象声明
ui.{h,cpp}         — 三屏 UI（动画屏、用量屏、蓝牙屏）；动画屏通过触摸切换，用量↔蓝牙由中键切换
splash.{h,cpp}     — 20×20 像素艺术动画引擎，24× 放大至 480×480
imu.{h,cpp}        — 加速度计驱动的旋转追踪器（返回 0..3）
power.{h,cpp}      — AXP2101 封装（电量百分比、充电状态、VBUS、电源键）
touch.{h,cpp}      — 简易点击检测 → ui_toggle_splash()（用量/动画切换）或 ble_clear_bonds()（蓝牙重置区域）
ble.{h,cpp}        — NimBLE 外设：自定义数据服务 + HID 键盘
usage_rate.{h,cpp} — 追踪 session_pct 在滑动窗口内的变化速率；返回分组 0..3（空闲→繁忙）
                     供 splash.cpp 选择对应情绪档位的动画
theme.h            — 设计令牌（UI 颜色的唯一来源 — Anthropic 风格深色/AMOLED 调色板）
data.h             — UsageData 结构体
icons.h            — 图标数组。电池图标（5 个）为带 Alpha 的 RGB565A8 格式；其余为原始 RGB565。
logo.h             — 80×80 RGB565 Logo
font_*.c           — 预编译的 LVGL 9 位图字体（Tiempos 34/56px，Styrene 12/14/16/20/24/28/48px，Mono 18/32px）
splash_animations.h — 自动生成，请勿手动编辑
```

## 构建 / 烧录

```bash
pio run -d firmware                   # 仅构建
./flash.sh                            # 构建 + 烧录（Linux，自动检测 /dev/ttyACM0）
./flash-mac.sh                        # 构建 + 烧录（macOS，自动检测 /dev/cu.usbmodem*）
./flash-mac.sh /dev/cu.usbmodem1101   # 指定端口
```

直接使用 PlatformIO：`pio run -d firmware -t upload --upload-port /dev/ttyACM0`

若 `pio` 不在 PATH 中，使用 `/home/hermann/.platformio/penv/bin/pio`。设备显示为 `/dev/ttyACM0`（Espressif USB JTAG），无需手动进入烧录模式。

## 自行验证 UI 改动——不要问用户

固件内置 `screenshot` 串口命令，可将 LVGL 帧缓冲区通过 `/dev/ttyACM0` 导出。`./screenshot.sh out.png /dev/ttyACM0` 可捕获一张 480×480 的 PNG。**每次 UI 迭代都要执行此操作**——用 Read 工具读取 PNG，目视确认改动，再继续迭代。

启动屏为 `SCREEN_SPLASH`，只有按下物理按键才能切换，因此新烧录后设备会停留在动画屏。若要截图正在编辑的屏幕而无需让用户按键，**临时修改 `main.cpp` 中的默认启动屏**（搜索 `ui_show_screen(SCREEN_SPLASH);`），改为 `SCREEN_USAGE` 或 `SCREEN_BLUETOOTH`，迭代完成后再改回来。

## 关键注意事项

1. **CO5300 不支持旋转。** 其 MADCTL 只支持轴翻转，不支持行列交换。旋转由 `main.cpp` 中 `my_flush_cb` 的 **CPU 像素重映射**完成。采用**分块渲染模式配合条带旋转**（480×40 小条带，速度快）。旋转切换时 → AMOLED 亮度闪烁 → 强制重绘。
2. **必须使用 OPI PSRAM：** platformio.ini 中须设置 `board_build.arduino.memory_type = qio_opi`。否则 `MALLOC_CAP_SPIRAM` 返回 NULL，屏幕全黑。
3. **必须使用 pioarduino 平台。** GFX Library for Arduino 需要 Arduino Core 3.x（`esp32-hal-periman.h`），而标准 `espressif32` 平台附带的是 2.x。我们锁定 `pioarduino/platform-espressif32` 55.03.38-1。
4. **LVGL 9 字体补丁。** `lv_font_conv` 输出 LVGL 8 格式，需手动修改：删除 `#if LVGL_VERSION_MAJOR >= 8` 守卫、去掉 `.cache` 字段、添加 `.release_glyph`、`.kerning`、`.static_bitmap`、`.fallback`、`.user_data`。不打补丁则字体编译通过但渲染不可见。
5. **触摸读取必须集中处理。** CST9220 的 `getPoint()` 会发起完整 I2C 事务。在多处调用会互相消耗数据导致输入失效。`touch_read()` 在 `main.cpp` 的每次 loop 中只调用一次；LVGL 的 `my_touch_cb` 和 `touch.cpp` 均从共享的 `touch_pressed/touch_x/touch_y` 状态读取。
6. **CO5300 需要对齐到偶数的刷新区域。** `rounder_cb` 负责强制对齐。
7. **触摸的 `setSwapXY(true)` 和 `setMirrorXY(true, false)`** 是默认旋转 0° 时经实测正确的值。IMU 旋转逻辑不改变触摸映射（它在 CPU 侧旋转渲染像素，LVGL 始终认为显示屏处于竖屏 0° 状态）。
8. **LVGL RGB565A8 为平面格式。** `w*h` 个 RGB565 像素后接 `w*h` 个 Alpha 字节；`data_size = w*h*3`，`stride = w*2`。对需要叠加在非纯色背景上的图标（如电池叠加在动画上）使用 `init_icon_dsc_rgb565a8()`。Lucide 源 PNG 为黑色透明底——转换器必须着色为白色，否则图标不可见。参见 `tools/png_to_lvgl.js`。

## 图标

`tools/png_to_lvgl.js <input.png> <symbol> [W_MACRO] [H_MACRO] [--tint=RRGGBB | --no-tint]` 将带 Alpha 的 PNG 转换为 RGB565A8。默认着色为白色（`0xFFFFFF`）——Lucide PNG 必须着色。输出内容拼入 `firmware/src/icons.h`，并在 ui.cpp 中使用 `init_icon_dsc_rgb565a8()`。目前只有 5 个电池图标使用此格式；其余图标仍为烧制在面板背景色上的原始 RGB565，因为它们都位于纯色区域内，这样处理没问题。

## 动画屏动画

13 组 20×20 像素艺术角色动画，来源于 [claudepix.vercel.app](https://claudepix.vercel.app)。处理流程：

```bash
node tools/scrape_claudepix.js  # → tools/claudepix_data/*.json
node tools/convert_to_c.js      # → firmware/src/splash_animations.h
```

每组动画有一个 10 色 RGB565 调色板，帧数据中的 0..9 为调色板索引。为默认启动屏幕。动画按 `usage_rate_group()` 分组（0=空闲，1=正常，2=活跃，3=繁忙），动画屏情绪随 Claude 使用强度变化。

## 用户画像 / 偏好

请参阅 `~/.claude/projects/.../memory/` 中的持久化上下文（用户是嵌入式入门级高级开发者，注重品牌一致性，偏好迭代式 UI 优化，不希望 AI 自行创作本应由第三方提供的美术资产）。每次会话开始时必须读取这些记忆文件。

## 守护进程 / 宿主机

两种守护进程实现——Linux（Bash）和 macOS（Python）：

| 平台 | 脚本 | 服务管理 |
|---|---|---|
| Linux | `daemon/claude-usage-daemon.sh` | systemd 用户服务，通过 `./install.sh` 安装 |
| macOS | `daemon/claude_usage_daemon.py` | LaunchAgent，通过 `./install-mac.sh` 安装 |

Linux 从 `~/.claude/.credentials.json` 读取 OAuth Token；macOS 从 Keychain（`Claude Code-credentials`）读取。

Linux 启动：`systemctl --user start claude-usage-daemon`。Unit 文件的 `ExecStart` 为绝对路径——切换工作树时须重新指向。

**发现与重连机制：**

- 首次运行时按名称（`"Claude Controller"`）连接，并将解析到的 MAC 缓存至 `~/.config/claude-usage-monitor/ble-address`。ESP32 的 BLE 地址为出厂固化，更换板子后缓存失效。
- 连接失败时：删除缓存，同时从 bluez 中移除设备（`bluetoothctl remove`），避免下次扫描复用已失效的 MAC。
- `POLL_INTERVAL=60`，`TICK=5`。内层循环每 5 秒唤醒一次以快速检测断连；每 60 秒或 ESP32 发送刷新请求时轮询 Anthropic。

**服务 `4c41555a-...0001` 上的 GATT 特征：**

- `...0002` RX — 守护进程将 JSON 用量数据写入此处。
- `...0003` TX — 固件发送 ack/nack 通知（守护进程不订阅）。
- `...0004` REQ — 若 `has_received_data` 为 false，固件在 `onSubscribe` 时发送 `0x01` 通知。守护进程通过 `setsid bash -c "stdbuf -oL dbus-monitor … | awk …"` 订阅；awk 将标志文件落盘，内层循环检测该文件。三个细节陷阱（管道缓冲、busctl 退出竞争、`wait` 阻塞管道任务）见 `feedback_dbus_monitor_pipe` 记忆文件。
