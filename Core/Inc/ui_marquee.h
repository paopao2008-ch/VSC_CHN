/*==============================================================================
 * ui_marquee.h —— 中文跑马灯界面（16x16 点阵）
 *
 * 效果：
 *   屏幕上下居中排两行 16x16 点阵文字，
 *     第 1 行（上）从左向右滑动，循环；
 *     第 2 行（下）从右向左滑动，循环。
 *
 * 两行的区别：
 *   · 第 1 行是**静态文字**，编译期就定死（UI_MARQUEE_LINE1）；
 *   · 第 2 行是**动态文字**，由调用方每帧传进来，本项目里传的是
 *     "现在是HH:MM:SS"（前缀 + RTC 实时时间）。
 *     传 NULL 就退回到静态的 UI_MARQUEE_LINE2。
 *
 * 说明：
 *   · 文字用 UTF-8 直接写在下面的宏里，字模由 Scripts/gen_font_cn.py 生成。
 *     改了文字如果里面出现字模里没有的字，屏幕上会用空心方框代替 ——
 *     把新字加进脚本的 CN_CHARS 重新生成即可。
 *   · 无缝循环：文本宽度 + 间隙 = 一个循环周期，屏幕外会预先画好下一份，
 *     所以看不到"跳回原点"。
 *============================================================================*/
#ifndef __UI_MARQUEE_H
#define __UI_MARQUEE_H

#include <stdint.h>

/*------------------------------------------------------------------------------
 * 显示内容（UTF-8）
 *----------------------------------------------------------------------------*/
/* 第 1 行：静态文字，编译期定死 */
#ifndef UI_MARQUEE_LINE1
#define UI_MARQUEE_LINE1     "您好，我是STM32"
#endif

/* 第 2 行：默认显示"现在是HH:MM:SS"。
 *   前缀 + 时分秒由 UI_MarqueeFormatTime() 拼出来，main.c 每 100ms 刷新一次；
 *   只有调用方传 NULL 时才用下面这条静态兜底。 */
#ifndef UI_MARQUEE_TIME_PREFIX
#define UI_MARQUEE_TIME_PREFIX   "现在是"         /* 想改"此刻是/当前时间"改这里 */
#endif
/* RTC 秒计数器不动时显示的内容（一眼分清是字模问题还是 RTC 停了） */
#ifndef UI_MARQUEE_TIME_STALL
#define UI_MARQUEE_TIME_STALL    "--:--:--"
#endif
/* 静态兜底（同时也是算第 2 行循环周期用的模板，宽度必须和真实内容一致） */
#ifndef UI_MARQUEE_LINE2
#define UI_MARQUEE_LINE2     "现在是00:00:00"
#endif

/* 第 2 行文本缓冲区大小：前缀最多按 8 个汉字算（8*3 字节）+ 8 位时分秒 + 结尾 0 */
#ifndef UI_MARQUEE_LINE2_BUF
#define UI_MARQUEE_LINE2_BUF     40u
#endif

/*------------------------------------------------------------------------------
 * 滚动参数
 *----------------------------------------------------------------------------*/
/* 每移动 1 像素需要的毫秒数：数越小滚得越快（40 ≈ 25 像素/秒） */
#ifndef UI_MARQUEE_STEP_MS
#define UI_MARQUEE_STEP_MS   40u
#endif

/* 两行之间的空隙（像素）。16 + 空隙 + 16 这一块整体在 64 行里垂直居中 */
#ifndef UI_MARQUEE_LINE_GAP
#define UI_MARQUEE_LINE_GAP  8u
#endif

/* 循环时前后两份文字之间的空隙（像素）。要保证 文本宽+空隙 > 128，
 * 否则屏幕上会同时出现两份文字。 */
#ifndef UI_MARQUEE_LOOP_GAP
#define UI_MARQUEE_LOOP_GAP  32u
#endif

/*------------------------------------------------------------------------------
 * 汉字/ASCII 的横向步进（含 1 像素字符间隙，避免笔画连在一起）
 *----------------------------------------------------------------------------*/
#define MARQUEE_CN_ADV       16u
#define MARQUEE_EN_ADV       9u

void     UI_MarqueeInit(void);

/* 每帧调用：TickMs = HAL_GetTick()，Line2 = 第 2 行文本（传 NULL 用静态兜底）。
 *   返回 1 = 本帧重画了；0 = 位置和文字都没变，省掉一次整屏推送。
 *   ⚠ Line2 指向的缓冲区要在调用期间一直有效（main.c 里用的是静态数组）。 */
uint8_t  UI_MarqueeShow(uint32_t TickMs, const char *Line2);

/* 拼"前缀 + HH:MM:SS"：Valid = 0 时改成"前缀 + --:--:--"。
 * 不用 snprintf —— 省掉 stdio，也就不会有 %02u 在各编译器上的歧义。 */
void     UI_MarqueeFormatTime(char *Buf, uint16_t Size,
                              uint8_t Hour, uint8_t Minute, uint8_t Second,
                              uint8_t Valid);

uint16_t UI_MarqueeTextWidth(const char *Utf8);

#endif /* __UI_MARQUEE_H */
