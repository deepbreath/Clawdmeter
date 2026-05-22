# Clawdmeter

[English](README.md) | [中文](README.zh.md)

Clawdmeter 是一个放在桌面上的 ESP32 小仪表盘，用来查看 Claude Code 用量、Codex/OpenAI 额度、蓝牙连接状态，并播放 Clawd 像素动画。

它运行在 [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm?&aff_id=149786) 上，通过 BLE 与电脑上的守护进程通信。两侧按键还能作为 BLE HID 键盘发送 Space 和 Shift+Tab，方便 Claude Code 语音模式和模式切换。

|              用量仪表              |              Clawd 动画屏              |
| :--------------------------------: | :------------------------------------: |
| ![Usage meter](assets/demo.jpeg) | ![Clawd animation screen](assets/demo.gif) |

Clawd 动画来自 [claudepix](https://claudepix.vercel.app)，由 [@amaanbuilds](https://x.com/amaanbuilds) 制作。

## 新功能

- **中英文文档切换：** README 顶部提供 English / 中文入口。
- **Codex 用量屏：** 守护进程可读取 `OPENAI_API_KEY`、`~/.codex/config.json` 或 `~/.codex/auth.json`，把 Codex/OpenAI token 与 request 剩余额度发送到设备。
- **屏幕循环更新：** 中键现在在 Usage、Codex、Bluetooth 之间切换；动画屏仍可触摸呼出/隐藏。
- **Wi-Fi 状态：** 固件可从 SD 卡读取 Wi-Fi 配置，并在仪表盘上显示连接质量。
- **录音与备份：** 双击右侧按键 `GPIO18` 或发送串口命令 `rec` 可录制 WAV 到 SD 卡；Wi-Fi 连上后可自动上传 pending 录音。
- **声音通知：** Claude Code hook 事件可触发板载 ES8311 播放 start、complete、input、error 提示音。
- **主机音频流：** `daemon/fish_voice.py` 可将 Opus/ADPCM 音频帧通过 BLE 写入设备；配置 Wi-Fi 音频目标后可走 TCP 传输。

## 屏幕

设备开机进入 Clawd 动画屏。按中键进入仪表盘并循环：

1. Usage：Claude 5 小时 session 与 7 天周额度。
2. Codex：Codex/OpenAI token 与 request 剩余额度。
3. Bluetooth：BLE 连接状态、MAC 地址和 bond reset。

|              Splash               |              Usage              |                Bluetooth                |
| :-------------------------------: | :-----------------------------: | :-------------------------------------: |
| ![Splash](screenshots/splash.png) | ![Usage](screenshots/usage.png) | ![Bluetooth](screenshots/bluetooth.png) |

动画屏会根据 Claude 使用强度选择不同情绪组，并在当前组内自动轮换动画。

## 硬件

- Waveshare ESP32-S3-Touch-AMOLED-2.16：ESP32-S3R8、2.16 英寸 480x480 AMOLED、CST9220 触摸、AXP2101 电源管理、QMI8658 IMU。
- USB-C 线：用于烧录和充电。
- 3.7V Li-Po 电池：MX1.25 2-pin，可选。
- microSD 卡：录音、Wi-Fi 配置、录音上传备份需要。

## 安装概览

### macOS

```bash
./flash-mac.sh
./install-mac.sh
```

烧录后在 **System Settings > Bluetooth** 连接 Clawdmeter。守护进程从 macOS Keychain 的 `Claude Code-credentials` 读取 Claude OAuth token。

### Windows

```powershell
pio run -d firmware -t upload --upload-port COM4
pip install bleak httpx
$env:PYTHONUTF8=1; python daemon/claude_usage_daemon.py
```

先在 **Settings > Bluetooth & devices** 中连接 "Claude Controller"；配对后建议从 Windows 蓝牙设置中移除它，让 bleak 守护进程直接扫描和连接。

### Linux

```bash
cd firmware
pio run -t upload --upload-port /dev/ttyACM0
./install.sh
systemctl --user start claude-usage-daemon
```

守护进程默认读取 `~/.claude/.credentials.json`。

## Claude 与 Codex 数据

守护进程会发起一个极小的 Claude API 请求，从响应头读取 rate-limit 用量，并通过 BLE 写入固件。可选的 Codex/OpenAI 数据来自 OpenAI rate-limit 响应头：

```json
{ "s": 45, "sr": 120, "w": 28, "wr": 7200, "st": "allowed", "ok": true, "cx_ts": 80, "cx_tr": 30, "cx_rs": 95, "cx_ok": true, "evt": 2 }
```

字段含义：

- `s` / `sr`：Claude session 用量百分比与重置分钟数。
- `w` / `wr`：Claude weekly 用量百分比与重置分钟数。
- `cx_ts` / `cx_tr`：Codex token 剩余百分比与重置秒数。
- `cx_rs`：Codex request 剩余百分比。
- `evt`：可选声音事件，`1=start`、`2=complete`、`3=error`、`4=input`。

## 录音、Wi-Fi 与 SD 备份

录音文件保存在 SD 卡：

- 待上传：`/recordings/pending/*.wav` 和同名 `.json`
- 已上传：`/recordings/sent/*.wav` 和同名 `.json`

Wi-Fi 配置：

```json
// /config/wifi.json
{
  "ssid": "your-wifi",
  "password": "your-password",
  "device_id": "clawdmeter-001"
}
```

上传配置：

```json
// /config/recorder.json
{
  "upload_url": "http://raspberrypi.local:8080/upload",
  "auth_token": "optional-token"
}
```

常用串口命令：

- `rec`：开始/停止录音
- `recplay`：播放最近一次录音
- `recanalyze`：分析最近一次 WAV
- `upload`：请求扫描并上传 pending 录音
- `wifi`：打印 Wi-Fi 和上传状态
- `sound 2`：直接播放提示音，编号见上面的 `evt`

更多细节见 [docs/recording-backup.md](docs/recording-backup.md) 和 [docs/录音文档.md](docs/录音文档.md)。

## BLE 协议

|                            | UUID                                   |
| -------------------------- | -------------------------------------- |
| Data Service               | `4c41555a-4465-7669-6365-000000000001` |
| RX Characteristic (write)  | `4c41555a-4465-7669-6365-000000000002` |
| TX Characteristic (notify) | `4c41555a-4465-7669-6365-000000000003` |
| REQ Characteristic (notify) | `4c41555a-4465-7669-6365-000000000004` |
| AUDIO Characteristic (write) | `4c41555a-4465-7669-6365-000000000005` |
| HID Service                | `00001812-0000-1000-8000-00805f9b34fb` |

## 鸣谢与许可提醒

- Clawd 像素动画来自 [claudepix.vercel.app](https://claudepix.vercel.app)。
- UI 图标使用 [Lucide](https://lucide.dev)。
- 仓库中包含 Anthropic 品牌字体和 Clawd 相关素材，存在授权灰区；fork 或复用前请留意原 README 的 licensing warning。
