/*==============================================================================
 * ui_marquee.c —— 中文跑马灯界面（16x16 点阵）
 *
 * 关键设计：
 *   1. 文本宽度 + 循环空隙 = 一个循环周期 Stride。
 *      绘制时从"起始 x"开始，每隔 Stride 画一份，直到超出屏幕右边界。
 *      因为是按 Stride 取模算位置的，所以两份之间天然无缝，看不到跳回原点。
 *   2. 上行（左->右）：X0 = Offset - Stride，Offset 增大时文字整体右移；
 *      下行（右->左）：X0 = -Offset，Offset 增大时文字整体左移。
 *   3. 第 1 行静态、第 2 行动态（"现在是HH:MM:SS"）。动态那行用副本比对来发现
 *      "文字变了"：变了就强制重画，不然时间要等到下一次像素步进才更新到屏幕上。
 *   4. 完全不使用浮点、不使用除法以外的重型运算，F103 上稳跑 25fps。
 *============================================================================*/
#include "ui_marquee.h"
#include "oled_ssd1315.h"
#include "font_cn.h"

/*------------------------------------------------------------------------------
 * 垂直布局：两行 16 像素 + 中间空隙，整块在 64 行里居中
 *----------------------------------------------------------------------------*/
#define MARQUEE_BLOCK_H   ((uint16_t)(CN_FONT_H + UI_MARQUEE_LINE_GAP + CN_FONT_H))
#define MARQUEE_TOP_Y     ((int16_t)(((int16_t)OLED_HEIGHT - (int16_t)MARQUEE_BLOCK_H) / 2))
#define MARQUEE_LINE1_Y   (MARQUEE_TOP_Y)
#define MARQUEE_LINE2_Y   ((int16_t)(MARQUEE_TOP_Y + (int16_t)CN_FONT_H + (int16_t)UI_MARQUEE_LINE_GAP))

/*------------------------------------------------------------------------------
 * 小工具：定长字符串拷贝 / 比较（不引 string.h，避免和 minlibc 打架）
 *----------------------------------------------------------------------------*/
static uint16_t MarqueeStrCopy(char *Dst, uint16_t Size, const char *Src)
{
    uint16_t i = 0u;

    if (Size == 0u) { return 0u; }
    while ((Src[i] != '\0') && (i < (uint16_t)(Size - 1u)))
    {
        Dst[i] = Src[i];
        i++;
    }
    Dst[i] = '\0';
    return i;
}

static uint8_t MarqueeStrEqual(const char *A, const char *B)
{
    uint16_t i = 0u;

    while (A[i] == B[i])
    {
        if (A[i] == '\0') { return 1u; }
        i++;
    }
    return 0u;
}

/*------------------------------------------------------------------------------
 * 拼第 2 行文本："前缀" + "HH:MM:SS"（Valid = 0 时后半段换成 "--:--:--"）
 *
 * 为什么不用 snprintf("%02u")：
 *   一是省掉 stdio（F103C8T6 只有 64KB Flash，stdio 一进来就胖一圈）；
 *   二是各编译器对 %02u 的实现细节（尤其是 ARMCC5 的 microlib）并不完全一致，
 *   手搓个位十位反而最稳，也不受 locale 影响。
 *----------------------------------------------------------------------------*/
void UI_MarqueeFormatTime(char *Buf, uint16_t Size,
                          uint8_t Hour, uint8_t Minute, uint8_t Second,
                          uint8_t Valid)
{
    uint16_t p;

    if ((Buf == (char *)0) || (Size == 0u)) { return; }

    p = MarqueeStrCopy(Buf, Size, UI_MARQUEE_TIME_PREFIX);

    if (Valid == 0u)
    {
        (void)MarqueeStrCopy(&Buf[p], (uint16_t)(Size - p), UI_MARQUEE_TIME_STALL);
        return;
    }

    /* 还放得下 8 位 "HH:MM:SS" + 结尾 0 才继续写，绝不越界 */
    if ((uint16_t)(p + 9u) > Size) { return; }

    Buf[p + 0u] = (char)('0' + (uint8_t)(Hour   / 10u));
    Buf[p + 1u] = (char)('0' + (uint8_t)(Hour   % 10u));
    Buf[p + 2u] = ':';
    Buf[p + 3u] = (char)('0' + (uint8_t)(Minute / 10u));
    Buf[p + 4u] = (char)('0' + (uint8_t)(Minute % 10u));
    Buf[p + 5u] = ':';
    Buf[p + 6u] = (char)('0' + (uint8_t)(Second / 10u));
    Buf[p + 7u] = (char)('0' + (uint8_t)(Second % 10u));
    Buf[p + 8u] = '\0';
}

/*------------------------------------------------------------------------------
 * 文本像素宽度：汉字 16、半角 ASCII 9（8 宽 + 1 间隙）
 *----------------------------------------------------------------------------*/
uint16_t UI_MarqueeTextWidth(const char *Utf8)
{
    uint16_t    w = 0u;
    const char *p = Utf8;

    while (*p != '\0')
    {
        uint32_t code = 0u;
        uint8_t  n    = CN_Utf8Next(p, &code);

        w = (uint16_t)(w + ((code < 0x80u) ? MARQUEE_EN_ADV : MARQUEE_CN_ADV));
        p += n;
    }
    return w;
}

/*------------------------------------------------------------------------------
 * 在 (X, Y) 处画一整串文字（UTF-8），超出屏幕的部分自动裁掉
 *----------------------------------------------------------------------------*/
static void MarqueeDrawText(int16_t X, int16_t Y, const char *Utf8)
{
    int16_t     x = X;
    const char *p = Utf8;

    while (*p != '\0')
    {
        uint32_t code = 0u;
        uint8_t  n    = CN_Utf8Next(p, &code);

        if (code < 0x80u)
        {
            OLED_DrawBitmap(x, Y, CN_Font_Ascii((uint8_t)code),
                            (uint8_t)CN_ASCII_W, (uint8_t)CN_ASCII_H, OLED_COLOR_WHITE);
            x = (int16_t)(x + (int16_t)MARQUEE_EN_ADV);
        }
        else
        {
            const uint8_t *glyph = CN_Font_Find(code);

            if (glyph != (const uint8_t *)0)
            {
                OLED_DrawBitmap(x, Y, glyph, (uint8_t)CN_FONT_W, (uint8_t)CN_FONT_H, OLED_COLOR_WHITE);
            }
            else
            {
                /* 字模里没有这个字：画个空心方框，一眼就能看出是缺字而非显示故障 */
                OLED_DrawRect((int16_t)(x + 1), (int16_t)(Y + 1),
                              (int16_t)(CN_FONT_W - 2), (int16_t)(CN_FONT_H - 2), OLED_COLOR_WHITE);
            }
            x = (int16_t)(x + (int16_t)MARQUEE_CN_ADV);
        }

        p += n;
        if (x >= (int16_t)OLED_WIDTH) { break; }   /* 后续字符已完全在屏幕外 */
    }
}

/*------------------------------------------------------------------------------
 * 画一整行滚动的文字：从 StartX 起，每隔 Stride 画一份，铺满屏幕
 *----------------------------------------------------------------------------*/
static void MarqueeDrawScrolling(int16_t Y, const char *Utf8, uint16_t Stride, uint16_t Offset, uint8_t ToRight)
{
    int32_t x;

    if (Stride == 0u) { return; }

    if (ToRight != 0u)
    {
        x = (int32_t)Offset - (int32_t)Stride;     /* 左 -> 右：从屏幕左侧进入 */
    }
    else
    {
        x = -(int32_t)Offset;                      /* 右 -> 左：从屏幕右侧进入 */
    }

    while (x < (int32_t)OLED_WIDTH)
    {
        MarqueeDrawText((int16_t)x, Y, Utf8);
        x += (int32_t)Stride;
    }
}

/*------------------------------------------------------------------------------
 * 初始化：算出两行的宽度与循环周期，清一次屏
 *   不调用也能工作（Show 里会懒初始化），显式调用只是为了第一帧就干净
 *----------------------------------------------------------------------------*/
static uint16_t s_Line1Stride = 0u;
static uint16_t s_Line2Stride = 0u;

/* 第 2 行是动态的：这里留一份上一帧文本的副本，用来判断"文字变了没"。
 * 变了就要 (1) 重算循环周期 (2) 强制重画 —— 否则会出现
 * "时间在后台更新了，但屏幕要等到下一次像素步进才跟着动" 的迟滞。 */
static char        s_Line2Copy[UI_MARQUEE_LINE2_BUF];
static const char *s_Line2Cur  = UI_MARQUEE_LINE2;   /* 当前真正拿去画的那份 */
static uint8_t     s_Line2Has  = 0u;                 /* s_Line2Copy 里有有效内容吗 */

void UI_MarqueeInit(void)
{
    UI_MarqueeTextWidth(UI_MARQUEE_LINE1);        /* 保证字模/解码路径被走到，便于定位问题 */

    s_Line1Stride = (uint16_t)(UI_MarqueeTextWidth(UI_MARQUEE_LINE1) + UI_MARQUEE_LOOP_GAP);
    s_Line2Stride = (uint16_t)(UI_MarqueeTextWidth(UI_MARQUEE_LINE2) + UI_MARQUEE_LOOP_GAP);

    s_Line2Cur = UI_MARQUEE_LINE2;
    s_Line2Has = 0u;

    OLED_Clear();
    OLED_GRAM_Refresh();
}

/*------------------------------------------------------------------------------
 * 换第 2 行文本：内容变了才动，顺便重算循环周期
 *   返回 1 = 内容有变化（调用方必须重画）
 *----------------------------------------------------------------------------*/
static uint8_t MarqueeLine2Update(const char *Line2)
{
    if (Line2 == (const char *)0) { return 0u; }        /* NULL -> 继续用静态兜底 */

    if ((s_Line2Has != 0u) && (MarqueeStrEqual(s_Line2Copy, Line2) != 0u))
    {
        return 0u;                                      /* 一个字都没变 */
    }

    (void)MarqueeStrCopy(s_Line2Copy, (uint16_t)UI_MARQUEE_LINE2_BUF, Line2);
    s_Line2Has = 1u;
    s_Line2Cur = Line2;
    s_Line2Stride = (uint16_t)(UI_MarqueeTextWidth(Line2) + UI_MARQUEE_LOOP_GAP);
    return 1u;
}

/*------------------------------------------------------------------------------
 * 每帧调用（建议主循环 5~20ms 调一次）
 *   返回 1 表示这一帧真的有位移、已经重画并推送；
 *   返回 0 表示位置没变，什么都没干（省掉一次 1KB 的 I2C 整屏推送）。
 *
 * 计时方式：用两次调用之间的**时间差**累加，而不是直接拿 TickMs/STEP_MS ——
 * HAL_GetTick() 会在开机约 49.7 天时回绕，差值法（uint32 减法）天然正确，
 * 而且不要求 TickMs 从 0 开始。位移量 s_Offset 单调累加、绘制时才取模，
 * 所以要跑 5 年多才会碰到它自己的回绕。
 *----------------------------------------------------------------------------*/
uint8_t UI_MarqueeShow(uint32_t TickMs, const char *Line2)
{
    static uint32_t s_LastTick   = 0u;
    static uint32_t s_AccMs      = 0u;               /* 不足一个 STEP 的余量，留着下次用 */
    static uint32_t s_Offset     = 0u;               /* 累计移动的像素数 */
    static uint32_t s_LastOffset = 0xFFFFFFFFu;      /* 保证第一帧一定重画 */
    static uint8_t  s_Started    = 0u;
    uint16_t p1, p2;
    uint8_t  textChanged;

    if (s_Line1Stride == 0u)
    {
        UI_MarqueeInit();
    }

    textChanged = MarqueeLine2Update(Line2);         /* 第 2 行文字变了就强制重画 */

    if (s_Started == 0u)
    {
        s_Started  = 1u;
        s_LastTick = TickMs;                         /* 第一次只建基准，不推进位移 */
    }
    else
    {
        s_AccMs += (uint32_t)(TickMs - s_LastTick);
        s_LastTick = TickMs;

        while (s_AccMs >= UI_MARQUEE_STEP_MS)
        {
            s_AccMs -= UI_MARQUEE_STEP_MS;
            s_Offset++;
        }
    }

    if ((s_Offset == s_LastOffset) && (textChanged == 0u))
    {
        return 0u;                                   /* 位置没变、文字也没变，连屏都不刷 */
    }
    s_LastOffset = s_Offset;

    p1 = (uint16_t)(s_Offset % s_Line1Stride);
    p2 = (uint16_t)(s_Offset % s_Line2Stride);

    OLED_Clear();
    MarqueeDrawScrolling(MARQUEE_LINE1_Y, UI_MARQUEE_LINE1, s_Line1Stride, p1, 1u);  /* 上行：左 -> 右 */
    MarqueeDrawScrolling(MARQUEE_LINE2_Y, s_Line2Cur,       s_Line2Stride, p2, 0u);  /* 下行：右 -> 左 */
    OLED_GRAM_Refresh();

    return 1u;
}
