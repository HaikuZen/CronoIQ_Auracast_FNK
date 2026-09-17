#ifndef _TFT_DISPLAY_H_
#define _TFT_DISPLAY_H_

#include "lvgl.h"
#include "TFT_Config.h"

#define TFT_BL 2

#if defined(FNK0115L_4_3_IPS) || defined(FNK0115N_5_0_TN) || defined(FNK0115Q_5_0_IPS)
  #define LCD_WIDTH  800
  #define LCD_HEIGHT 480
#elif defined(FNK0115B_4_3_TN)
  #define LCD_WIDTH  480
  #define LCD_HEIGHT 272
#endif

#define PIN_DE    40
#define PIN_VSYNC 41
#define PIN_HSYNC 39
#define PIN_PCLK  42
#define PIN_R0 45
#define PIN_R1 48
#define PIN_R2 47
#define PIN_R3 21
#define PIN_R4 14
#define PIN_G0 5
#define PIN_G1 6
#define PIN_G2 7
#define PIN_G3 15
#define PIN_G4 16
#define PIN_G5 4
#define PIN_B0 8
#define PIN_B1 3
#define PIN_B2 46
#define PIN_B3 9
#define PIN_B4 1

#if defined(FNK0115L_4_3_IPS) || defined(FNK0115Q_5_0_IPS)
  #define TOUCH_I2C_SDA 19
  #define TOUCH_I2C_SCL 20
  #define TOUCH_INT     18
  #define TOUCH_RST     38
#endif

// Initialises the RGB panel, backlight, GT911 touch, and the LVGL port
// (display + input device, when touch is available). Safe to call once
// before building any LVGL UI. Touch failure is non-fatal — see
// lvgl_display.cpp — the display still comes up without an input device.
void board_init(void);

#endif
