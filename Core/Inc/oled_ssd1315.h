/*==============================================================================
 * oled_ssd1315.h —— STM32F103C8T6 驱动 SSD1315 控制器 128x64 OLED（I2C 接口）
 *
 * 硬件：4Pin I2C 模块（VCC / GND / SCL / SDA）
 * 驱动：内部 128x64 显存(GRAM)，所有绘制先写 GRAM，最后 OLED_GRAM_Refresh() 一次性推送
 * 通信：软件模拟 I2C（bit bang），不依赖 HAL_I2C，换引脚只改下面几个宏
 *============================================================================*/
#ifndef __OLED_SSD1315_H
#define __OLED_SSD1315_H

#include <stdint.h>
#include "stm32f1xx_hal.h"

/*------------------------------------------------------------------------------
 * 引脚定义（软件 I2C，任意 GPIO 均可）
 *----------------------------------------------------------------------------*/
#ifndef OLED_SCL_PORT
#define OLED_SCL_PORT        GPIOB
#define OLED_SCL_PIN         GPIO_PIN_6
#endif
#ifndef OLED_SDA_PORT
#define OLED_SDA_PORT        GPIOB
#define OLED_SDA_PIN         GPIO_PIN_7
#endif

/*------------------------------------------------------------------------------
 * 参数定义
 *----------------------------------------------------------------------------*/
#define OLED_I2C_ADDR_7BIT   0x3C      /* 常见 0x3C；个别模块是 0x3D，改这里 */
#define OLED_CMD_BYTE        0x00      /* I2C 控制字节：命令流 */
#define OLED_DATA_BYTE       0x40      /* I2C 控制字节：数据流 */

#define OLED_WIDTH           128
#define OLED_HEIGHT          64
#define OLED_PAGE_HEIGHT     8
#define OLED_PAGE_NUM        (OLED_HEIGHT / OLED_PAGE_HEIGHT)

#define OLED_COLOR_BLACK     0
#define OLED_COLOR_WHITE     1

/*------------------------------------------------------------------------------
 * 功能函数
 *----------------------------------------------------------------------------*/
void        OLED_Init(void);
void        OLED_GPIO_Init(void);
void        OLED_Fill(uint8_t Color);
void        OLED_Clear(void);
void        OLED_SetCursor(uint8_t Page, uint8_t Col);

void        OLED_DrawPixel(int16_t X, int16_t Y, uint8_t Color);
void        OLED_DrawHLine(int16_t X0, int16_t X1, int16_t Y, uint8_t Color);
void        OLED_DrawVLine(int16_t X, int16_t Y0, int16_t Y1, uint8_t Color);
void        OLED_DrawRect(int16_t X, int16_t Y, int16_t W, int16_t H, uint8_t Color);

/* 通用位图：行优先、每行 ceil(W/8) 字节、高位在左（和字模生成脚本的输出一致） */
void        OLED_DrawBitmap(int16_t X, int16_t Y, const uint8_t *Bmp,
                            uint8_t W, uint8_t H, uint8_t Color);

void        OLED_PutChar(uint8_t X, uint8_t Y, char Ch, uint8_t Color);
void        OLED_Print(uint8_t X, uint8_t Y, const char *Str, uint8_t Color);
void        OLED_PrintCentered(int16_t X, int16_t Y, const char *Str, uint8_t Color);
void        OLED_PrintWidth(const char *Str, uint8_t *Width);

void        OLED_DrawBigChar(uint8_t X, uint8_t Y, char Ch, uint8_t Scale, uint8_t Bold, uint8_t Color);
void        OLED_DrawBigString(uint8_t X, uint8_t Y, const char *Str, uint8_t Scale, uint8_t Bold, uint8_t Color);
void        OLED_DrawBigStringCentered(int16_t Y, const char *Str, uint8_t Scale, uint8_t Bold, uint8_t Color);
void        OLED_BigStringAdvance(char Ch, uint8_t Scale, uint8_t Bold, uint8_t *Advance);

void        OLED_GRAM_Refresh(void);

#endif /* __OLED_SSD1315_H */
