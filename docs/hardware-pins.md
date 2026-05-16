# Waveshare ESP32-S3-Touch-AMOLED-2.16 引脚总览

来源：官方 `examples/Arduino-v3.3.5/libraries/Mylibrary/pin_config.h` + 示例代码

## 已使用引脚

| 外设 | 信号 | GPIO |
|------|------|------|
| CO5300 AMOLED（QSPI）| SDIO0 | 4 |
| CO5300 AMOLED（QSPI）| SDIO1 | 5 |
| CO5300 AMOLED（QSPI）| SDIO2 | 6 |
| CO5300 AMOLED（QSPI）| SDIO3 | 7 |
| CO5300 AMOLED（QSPI）| SCLK  | 38 |
| CO5300 AMOLED（QSPI）| RST   | 2 |
| CO5300 AMOLED（QSPI）| CS    | 12 |
| CST9220 触摸（I2C）  | SDA   | 15 |
| CST9220 触摸（I2C）  | SCL   | 14 |
| CST9220 触摸         | INT   | 11 |
| AXP2101 PMU（I2C，共用）| — | 15 / 14 |
| QMI8658 IMU（I2C，共用）| — | 15 / 14 |
| 左按键               | —     | 0 |
| 右按键               | —     | 18 |
| 中按键（PMU PKEY）   | IRQ   | AXP2101 内部 |

## 音频引脚

| 外设 | 信号 | GPIO |
|------|------|------|
| ES7210 麦克风 ADC（I2C）| SDA / SCL | 15 / 14（共用）|
| ES7210 麦克风 ADC       | I2C 地址 | 0x40 |
| ES7210（I2S）           | MCLK | 16 |
| ES7210（I2S）           | BCLK | 9 |
| ES7210（I2S）           | LRCK/WS | 45 |
| ES7210（I2S）           | DIN（数据→ESP32）| 10 |
| ES8311 喇叭编解码（I2C）| SDA / SCL | 15 / 14（共用）|
| ES8311 喇叭编解码       | I2C 地址 | 0x18 |
| ES8311（I2S）           | MCLK | 42 |
| ES8311（I2S）           | BCLK | 9（共用）|
| ES8311（I2S）           | LRCK/WS | 45（共用）|
| ES8311（I2S）           | DOUT（数据→喇叭）| 8 |
| 功率放大器使能           | PA_EN | 46 |

## TF 卡引脚（待确认）

原理图 PDF 需手动查阅（`schematic/ESP32-S3-Touch-AMOLED-2.16-Schematic.pdf`，搜索 "SD" 或 "TF"）。
SPI 模式预计 4 根线（MOSI/MISO/SCK/CS），从剩余可用 GPIO 中分配。

## 剩余可用 GPIO

以下引脚未被任何已知外设占用：

`1, 3, 13, 17, 19, 20, 21, 33, 34, 35, 36, 37, 39, 40, 41, 43, 44, 47, 48`

> 注意：GPIO 19/20 在部分设计中用作 USB D-/D+；GPIO 43/44 为 UART0 TX/RX。

## I2C 设备地址汇总

| 设备 | 地址 |
|------|------|
| CST9220 触摸 | 0x5A |
| AXP2101 PMU  | 0x34 |
| QMI8658 IMU  | 0x6B |
| ES7210 麦克风 ADC | 0x40 |
| ES8311 喇叭编解码 | 0x18 |
