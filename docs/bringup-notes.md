# Bring-up notes: SD, USB MSC, and audio

These are the debugged, working parameters for the Waveshare ESP32-S3-Touch-AMOLED-2.16 board used by this project.

## SD card

The onboard TF slot is SDMMC 1-bit, not SPI.

Working pins:

| Signal | GPIO |
|---|---:|
| SDMMC CLK | 2 |
| SDMMC CMD | 1 |
| SDMMC D0 | 3 |

Firmware API:

- Normal firmware SD access uses `SD_MMC.setPins(2, 1, 3)`.
- Mount with `SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 5)`.
- Success log:

```text
SD: 29827 MB ready
sd_phrases: group 0 - 20 phrases
sd_phrases: group 1 - 20 phrases
sd_phrases: group 2 - 20 phrases
sd_phrases: group 3 - 20 phrases
```

Important pin conflicts:

- GPIO2 is SDMMC CLK, so it must not be used as LCD/touch reset.
- GPIO6 is QSPI display SDIO2, so it must not be used as SD CS.
- The previous SPI setup (`CLK=0, MOSI=1, MISO=2, CS=6`) fails with `sdCommand(): Card Failed! cmd: 0x00`.

## USB MSC mode

USB MSC exposes the SD card as a USB drive for the PC.

Reliable entry:

1. Boot normally.
2. In PlatformIO serial monitor, type `msc` and press Enter.
3. The firmware stores an `RTC_NOINIT_ATTR` magic value and restarts into MSC mode.

Expected success log:

```text
[MSC] serial command received - rebooting into USB drive mode
[MSC] boot flag set - entering USB drive mode
[MSC] SD card OK: 61085696 sectors (29827 MB)
[MSC] forcing USB re-enumeration
{"mode":"usb_msc"}
[MSC] looping - SD owned by USB host
```

Implementation requirements:

- Use `RTC_NOINIT_ATTR` plus a magic value; `RTC_DATA_ATTR` was cleared during Arduino startup and did not survive the soft restart.
- Call `USB.begin()` after `USBMSC.begin(...)`; without this, SD init succeeds but Windows may not enumerate the disk.
- `USBMSC` callbacks use raw `sdmmc_read_sectors` and `sdmmc_write_sectors`.
- The `USBMSC` object must remain global/static so the MSC interface is included in the TinyUSB descriptor.

Exit MSC:

- Safely eject the disk in Windows first.
- If the board is on battery, unplugging USB may not reset it. Power-cycle the board or press reset/EN if available.

## Audio

Speaker output is ES8311 over I2S. Use the Arduino `ESP_I2S` wrapper, matching the Waveshare example path.

Working pins:

| Signal | GPIO |
|---|---:|
| ES8311 MCLK | 42 |
| ES7210 MCLK | 42 |
| I2S BCLK | 9 |
| I2S LRCK/WS | 45 |
| ES8311 DOUT (ESP32 -> codec) | 8 |
| PA_EN | 46 |

Working audio parameters:

- PCM sample rate: `16000 Hz`
- Bit depth: `16-bit`
- Slot mode: stereo, both slots (`I2S_SLOT_MODE_STEREO`, `I2S_STD_SLOT_BOTH`)
- MCLK: `16000 * 256 = 4.096 MHz`
- BCLK: `16000 * 2 slots * 16 bit = 512 kHz`
- ES8311 DAC volume register `0x32 = 0xB2` (about 70%)
- Firmware PCM gain: `2/3`
- Test tone gain: `0.45`
- PA enable: GPIO46 high after codec init

Serial test commands:

```text
tone
sound 1
sound 2
sound 3
sound 4
```

Success signs:

```text
I2S init OK
ES8311 chip ID: 0x83 (expect 0x83)
ES8311 init OK
sound: PA_EN=1, tone test
sd: play /phrases/group_0/018.wav (16000 Hz 1-ch, 3.8s)
```

Important audio gotcha:

- GPIO9 and GPIO10 must not be configured as buttons. GPIO9 is I2S BCLK and GPIO10 is mic DIN. Setting either with `pinMode(..., INPUT_PULLUP)` after audio init can break the I2S bus and cause silence.
