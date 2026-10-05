/* 5x7 点阵字模 头文件 —— 由 Scripts/gen_font5x7.py 生成 */
#ifndef __FONT5X7_H
#define __FONT5X7_H

#include <stdint.h>

#define FONT5X7_TAB_SIZE   95u    /* ' ' (0x20) ~ '~' (0x7E) */
#define FONT5X7_W          5u     /* 字宽  像素 */
#define FONT5X7_H          7u     /* 字高  像素 */
#define FONT5X7_ADV        6u     /* 字符步进（含 1 像素间隔） */

/* 把字符映射到字模下标（小写自动转大写） */
#define FONT5X7_INDEX(ch)  ((uint8_t)(((ch) >= 'a' && (ch) <= 'z') ? ((ch) - 32) : (ch)) - 0x20)

extern const uint8_t Font5x7[FONT5X7_TAB_SIZE][FONT5X7_H];

#endif
