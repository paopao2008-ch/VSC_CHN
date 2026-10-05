/*==============================================================================
 * font_cn.h —— 中文/ASCII 点阵字模（由 Scripts/gen_font_cn.py 自动生成）
 *============================================================================*/
#ifndef __FONT_CN_H
#define __FONT_CN_H

#include <stdint.h>

#define CN_FONT_W        16u     /* 汉字宽   */
#define CN_FONT_H        16u     /* 汉字高   */
#define CN_FONT_BYTES    32u     /* 每字字节数 */

#define CN_ASCII_W       8u      /* 半角 ASCII 宽 */
#define CN_ASCII_H       16u
#define CN_ASCII_BYTES   16u

typedef struct
{
    uint32_t       Code;          /* Unicode 码点   */
    const uint8_t *Data;          /* 32 字节点阵    */
} CN_GlyphType;

extern const CN_GlyphType CN_FontTable[];
extern const uint16_t     CN_FontCount;
extern const uint8_t      Font8x16[][CN_ASCII_BYTES];

const uint8_t *CN_Font_Find(uint32_t Code);
const uint8_t *CN_Font_Ascii(uint8_t Ch);
uint8_t        CN_Utf8Next(const char *S, uint32_t *Code);

#endif /* __FONT_CN_H */