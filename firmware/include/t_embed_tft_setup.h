#pragma once

// LilyGo T-Embed-CC1101 display wiring. The panel is an ST7789 170x320 and
// the board's repository routes TFT_eSPI through HSPI on ESP32-S3.
#define ST7789_DRIVER
#define TFT_WIDTH 170
#define TFT_HEIGHT 320
#define USE_HSPI_PORT
#define TFT_INVERSION_ON
#define TFT_BL 21
#define TFT_MISO 10
#define TFT_MOSI 9
#define TFT_SCLK 11
#define TFT_CS 41
#define TFT_DC 16
#define TFT_RST -1
#define SPI_FREQUENCY 40000000
#define SPI_READ_FREQUENCY 20000000
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
