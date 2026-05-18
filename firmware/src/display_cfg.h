#pragma once

#include <Arduino_GFX_Library.h>
#include <TouchDrvCSTXXX.hpp>
#include <XPowersLib.h>
#include <SensorQMI8658.hpp>
#include <Wire.h>

// ---- Display resolution ----
#define LCD_WIDTH   480
#define LCD_HEIGHT  480

// ---- QSPI display pins (CO5300) ----
#define LCD_CS      12
#define LCD_SCLK    38
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_RESET   39

// ---- Touch pins (CST9220 via I2C) ----
#define IIC_SDA     15
#define IIC_SCL     14
#define TP_INT      11
#define TP_RST      40
#define CST9220_ADDR 0x5A

// ---- PMU (AXP2101 via same I2C) ----
#define AXP2101_ADDR 0x34

// ---- Audio (ES8311 speaker codec + ES7210 mic ADC, shared I2S bus) ----
#define I2S_MCLK_ES8311  42
#define I2S_MCLK_ES7210  16
#define I2S_BCK          9
#define I2S_WS           45
#define I2S_DO           8    // ESP32 → ES8311 (speaker output)
#define I2S_DI           10   // ES7210 → ESP32 (mic input)
#define PA_EN            46   // power amplifier enable (active HIGH)
#define ES8311_ADDR      0x18
#define ES7210_ADDR      0x40

// ---- Global hardware objects (defined in main.cpp) ----
extern Arduino_DataBus *bus;
extern Arduino_CO5300 *gfx;
extern TouchDrvCST92xx touch;
extern XPowersPMU pmu;
extern SensorQMI8658 imu;
