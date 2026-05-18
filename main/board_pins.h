#pragma once

// Cardputer Adv LCD, ST7789V2, application-facing 240x135 landscape.
#define PIN_LCD_BL 38
#define PIN_LCD_RST 33
#define PIN_LCD_DC 34
#define PIN_LCD_MOSI 35
#define PIN_LCD_SCLK 36
#define PIN_LCD_CS 37

// Shared internal I2C bus: TCA8418 keyboard, BMI270 IMU, ES8311 codec control.
#define PIN_I2C_SDA 8
#define PIN_I2C_SCL 9
#define PIN_KBD_INT 11

// Battery ADC and IR transmitter.
#define PIN_BAT_ADC 10
#define PIN_IR_TX 44

// microSD and EXT SPI bus.
#define PIN_SD_CS 12
#define PIN_SPI_MOSI 14
#define PIN_SPI_SCLK 40
#define PIN_SPI_MISO 39

// Built-in audio I2S path.
#define PIN_I2S_BCLK 41
#define PIN_I2S_DIN 46
#define PIN_I2S_WS 43
#define PIN_I2S_DOUT 42

// EXT 14P pins, kept here to prevent accidental reuse.
#define PIN_EXT_RST 3
#define PIN_EXT_INT 4
#define PIN_EXT_BUSY 6
#define PIN_EXT_CS 5
#define PIN_EXT_UART_RX 13
#define PIN_EXT_UART_TX 15

#define LCD_NATIVE_WIDTH 135
#define LCD_NATIVE_HEIGHT 240
#define LCD_WIDTH 240
#define LCD_HEIGHT 135
#define LCD_GAP_X 40
#define LCD_GAP_Y 53
#define LCD_SWAP_XY 1
#define LCD_MIRROR_X 1
#define LCD_MIRROR_Y 0

#define I2C_PORT I2C_NUM_1
#define LCD_SPI_HOST SPI3_HOST
#define SD_SPI_HOST SPI2_HOST

#define I2C_ADDR_TCA8418 0x34
#define I2C_ADDR_BMI270 0x69
#define I2C_ADDR_ES8311 0x18
