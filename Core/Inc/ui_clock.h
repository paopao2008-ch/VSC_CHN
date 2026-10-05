/*==============================================================================
 * ui_clock.h —— OLED 时钟界面布局
 *
 * 布局（128 x 64）：
 *   y =  1 ~  7   顶栏：时钟源(LSE/LSI/NONE) 或告警 !RTC STOP，右上角 3x3 心跳块
 *   y =  8 ~ 29   大号粗体 HH:MM:SS（字号 = 5x7 点阵放大 3 倍 + 描粗，约 16x22 像素/字符）
 *   y = 34        分隔虚线
 *   y = 40 ~ 46   小号 5x7 字体：2026-10-04 SUN
 *   y = 52 ~ 53   秒进度条
 *   y = 56 ~ 62   底栏：复位次数 RST=n（超过 3 次会闪一个 PWR? 提醒）
 *============================================================================*/
#ifndef __UI_CLOCK_H
#define __UI_CLOCK_H

#include <stdint.h>
#include "rtc.h"

/* 大号时间的放大倍数：2 = 约 11x15，3 = 约 16x22（默认），4 时 8 个字符会放不下 */
#define UI_BIG_SCALE     3u

/* 大号时间是否描粗（向右补 1 像素，做出“粗体”效果） */
#define UI_BIG_BOLD      1u

/* 是否显示秒进度条（1 = 显示） */
#define UI_SHOW_SEC_BAR  1u

/* 界面模式 */
#define UI_MODE_NORMAL  0u      /* 正常走时 */
#define UI_MODE_WARN    1u      /* RTC 停摆：顶部打 !RTC STOP */
#define UI_MODE_HEAL    2u      /* 正在自愈 / 自愈提示 */

void UI_ClockShow(const RTC_TimeType *Time, uint8_t Heart);

/* RTC 连续若干秒没跳秒时调用，界面顶部追加一行 “!RTC STOP” 告警，
 * 正常显示的时间内容不变。Heart 传当前心跳状态。 */
void UI_ClockShowStalled(const RTC_TimeType *Time, uint8_t Heart);

/* 自愈过程/自愈提示页 */
void UI_ClockShowHeal(const RTC_TimeType *Time, uint8_t Heart);

/* 排障用：把 RTC 的原始寄存器打在屏幕上（main 里每 400ms 调一次持续刷新）。
 * “时间不动”时先看这一页：BDCR 里的 RTCSEL/LSERDY、APB1ENR 的 bit0、
 * PWR_CR 的 DBP、还有 CNT 到底有没有在涨，一眼就能定论。 */
void UI_ClockShowDiag(const RTC_DiagType *D);

#endif /* __UI_CLOCK_H */
