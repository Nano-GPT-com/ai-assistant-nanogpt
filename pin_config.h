#pragma once

#define XPOWERS_CHIP_AXP2101

#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 11
#define LCD_CS 12
#define LCD_RESET 8
#define LCD_WIDTH 410
#define LCD_HEIGHT 502

// TOUCH
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 38
#define TP_RESET 9

// ES8311
#define I2S_MCK_IO 16
#define I2S_BCK_IO 41
#define I2S_DI_IO 42
#define I2S_WS_IO 45
#define I2S_DO_IO 40

#define MCLKPIN             16
#define BCLKPIN             41
#define WSPIN               45
#define DOPIN               40
#define DIPIN               42
#define PA                  46

// SD
const int SDMMC_CLK = 2;
const int SDMMC_CMD = 1;
const int SDMMC_DATA = 3;
const int SDMMC_CS = 17;
