# Waveshare ESP32-S3-Touch-AMOLED-2.16 pin overview

Sources: Waveshare schematic and official BSP examples.

## Display and input

| Peripheral | Signal | GPIO |
|---|---:|---:|
| CO5300 AMOLED (QSPI) | SDIO0 | 4 |
| CO5300 AMOLED (QSPI) | SDIO1 | 5 |
| CO5300 AMOLED (QSPI) | SDIO2 | 6 |
| CO5300 AMOLED (QSPI) | SDIO3 | 7 |
| CO5300 AMOLED (QSPI) | SCLK | 38 |
| CO5300 AMOLED (QSPI) | RST | 39 |
| CO5300 AMOLED (QSPI) | CS | 12 |
| CST9220 touch (I2C) | SDA | 15 |
| CST9220 touch (I2C) | SCL | 14 |
| CST9220 touch | INT | 11 |
| CST9220 touch | RST | 40 |
| AXP2101 PMU (I2C) | SDA / SCL | 15 / 14 |
| QMI8658 IMU (I2C) | SDA / SCL | 15 / 14 |
| Middle button | BTN_MID / USB MSC boot hold | 18 |
| I2S audio | BCLK | 9 |
| I2S audio | mic DIN | 10 |

## TF card

The onboard TF card slot uses SDMMC 1-bit mode, not SPI mode.

| Signal | GPIO |
|---|---:|
| SDMMC CLK | 2 |
| SDMMC CMD | 1 |
| SDMMC D0 | 3 |

## Audio

| Peripheral | Signal | GPIO |
|---|---:|---:|
| ES7210 mic ADC (I2C) | SDA / SCL | 15 / 14 |
| ES7210 mic ADC | I2C address | 0x40 |
| ES7210 (I2S) | MCLK | 42 |
| ES7210 (I2S) | BCLK | 9 |
| ES7210 (I2S) | LRCK / WS | 45 |
| ES7210 (I2S) | DIN (data to ESP32) | 10 |
| ES8311 speaker codec (I2C) | SDA / SCL | 15 / 14 |
| ES8311 speaker codec | I2C address | 0x18 |
| ES8311 (I2S) | MCLK | 42 |
| ES8311 (I2S) | BCLK | 9 |
| ES8311 (I2S) | LRCK / WS | 45 |
| ES8311 (I2S) | DOUT (data from ESP32) | 8 |
| Power amplifier | PA_EN | 46 |

## I2C addresses

| Device | Address |
|---|---:|
| CST9220 touch | 0x5A |
| AXP2101 PMU | 0x34 |
| QMI8658 IMU | 0x6B |
| ES7210 mic ADC | 0x40 |
| ES8311 speaker codec | 0x18 |

## Notes

- GPIO 2 is SDMMC CLK and must not be reused as LCD or touch reset.
- GPIO 6 is QSPI display SDIO2 and must not be reused as SD card CS.
- GPIO 9 is I2S BCLK and must not be reused as a button.
- GPIO 10 is I2S mic DIN and must not be reused as a button.
- GPIO 19/20 are used by USB D-/D+ on some designs; GPIO 43/44 are UART0 TX/RX.
- See `docs/bringup-notes.md` for the debugged SD, USB MSC, and audio parameters.
