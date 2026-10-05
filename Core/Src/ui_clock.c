/*==============================================================================
 * ui_clock.c —— 时钟界面
 *   上半屏：大号粗体时间 HH:MM:SS（自动居中）
 *   下半屏：小号字体 年-月-日 + 星期
 *
 * ⚠ 这一版的关键改动：界面**每 100ms 都会被 main 重新画一遍**，
 *   不再是“秒数变了才画”。右上角有个 3x3 的方块每 200ms 翻转一次当心跳。
 *
 *   为什么这么折腾：之前“时间不动”有两种完全不同的病，而它们在这一张小屏上
 *   长得一模一样（都是画面定格），根本分不出来：
 *     A. RTC 真的停了        —— 心跳还在闪，秒不走   -> 修时钟源 / 预分频
 *     B. MCU 卡死/复位了      —— 心跳也定格不动       -> 修电源、栈、看门狗
 *   有了心跳，这两种情况一眼就分开了，不用猜。
 *============================================================================*/
#include <stdio.h>
#include "ui_clock.h"
#include "oled_ssd1315.h"
#include "font5x7.h"
#include "rtc.h"

#define UI_TIME_Y        8u      /* 大号时间顶端 y */
#define UI_DIV_Y         34u     /* 分隔线 y */
#define UI_DATE_Y        40u     /* 小字日期顶端 y */
#define UI_BAR_Y         52u     /* 秒进度条顶端 y */
#define UI_TOP_Y         1u      /* 顶栏 y：时钟源 / 告警 / 心跳 */
#define UI_BOT_Y         56u     /* 底栏 y：复位次数 / 自愈提示 */

#define UI_HEART_X       ((uint8_t)(OLED_WIDTH - 4u))   /* 心跳块左上角 x */
#define UI_HEART_Y       1u

static const char *const s_WeekName[] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };

/* 右上角心跳：On=1 实心，On=0 空心。程序活着就每 200ms 翻一次 */
static void UI_Heartbeat(uint8_t On)
{
    if (On)
    {
        OLED_DrawRect((int16_t)UI_HEART_X, (int16_t)UI_HEART_Y, 3, 3, 1);
    }
    else
    {
        OLED_DrawRect((int16_t)UI_HEART_X, (int16_t)UI_HEART_Y, 3, 3, 0);
    }
}

/* 虚线分隔 */
static void UI_DrawDashedLine(int16_t Y)
{
    int16_t x;
    for (x = 0; x < OLED_WIDTH; x += 2)
    {
        OLED_DrawPixel(x, Y, 1);
        OLED_DrawPixel((int16_t)(x + 1), (int16_t)(Y + 1), 1);
    }
}

/*------------------------------------------------------------------------------
 * 一整帧
 *   Mode  : NORMAL 正常 / WARN RTC 停摆告警 / HEAL 自愈中
 *   Heart : 心跳块当前显示状态（1=实心 0=空心）
 *----------------------------------------------------------------------------*/
static void UI_Render(const RTC_TimeType *Time, uint8_t Mode, uint8_t Heart)
{
    char buf[40];
    uint8_t width;

    OLED_Clear();

    /* ---------- 1. 中间大号粗体：时:分:秒 ---------- */
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
             (unsigned)Time->Hour, (unsigned)Time->Minute, (unsigned)Time->Second);
    OLED_DrawBigStringCentered(UI_TIME_Y, buf, UI_BIG_SCALE, UI_BIG_BOLD, 1);

    /* ---------- 2. 分隔线 ---------- */
    UI_DrawDashedLine(UI_DIV_Y);

    /* ---------- 3. 下方小号：年-月-日 + 星期 ---------- */
    snprintf(buf, sizeof(buf), "20%02u-%02u-%02u %s",
             (unsigned)Time->Year, (unsigned)Time->Month, (unsigned)Time->Date,
             s_WeekName[Time->Week & 7u]);
    OLED_PrintWidth(buf, &width);
    OLED_Print((uint8_t)(((int16_t)OLED_WIDTH - (int16_t)width) / 2), UI_DATE_Y, buf, 1);

    /* ---------- 4. 秒进度条（可选） ---------- */
#if UI_SHOW_SEC_BAR
    {
        uint16_t barW = (uint16_t)(((uint32_t)Time->Second * OLED_WIDTH) / 60u);
        OLED_DrawHLine(0, (int16_t)(OLED_WIDTH - 1), UI_BAR_Y, 0);            /* 底槽 */
        OLED_DrawHLine(0, (int16_t)(OLED_WIDTH - 1), (int16_t)(UI_BAR_Y + 1), 0);
        if (barW > 0u)
        {
            OLED_DrawHLine(0, (int16_t)(barW - 1u), UI_BAR_Y, 1);
            OLED_DrawHLine(0, (int16_t)(barW - 1u), (int16_t)(UI_BAR_Y + 1), 1);
        }
    }
#endif

    /* ---------- 5. 顶栏 ---------- */
    if (Mode == UI_MODE_WARN)
    {
        OLED_Print(4, UI_TOP_Y, "!RTC STOP", 1);
    }
    else if (Mode == UI_MODE_HEAL)
    {
        OLED_Print(4, UI_TOP_Y, "RTC-HEAL", 1);
    }
    else
    {
        /* 正常时顶栏左侧显示当前时钟源：LSE / LSI / NONE
         * NONE = RTCCLK 是空的，秒计数器必然一动不动 */
        const char *src = RTC_GetSourceName();
        OLED_Print(2, UI_TOP_Y, src, 1);
    }
    UI_Heartbeat(Heart);

    /* ---------- 6. 底栏：复位次数 / 自愈提示 ---------- */
    {
        uint32_t rst = RTC_GetResetCount();

        if (Mode == UI_MODE_HEAL)
        {
            OLED_Print(2, UI_BOT_Y, "HEALING..", 1);
        }
        else
        {
            snprintf(buf, sizeof(buf), "RST=%lu", (unsigned long)rst);
            OLED_Print(2, UI_BOT_Y, buf, 1);

            /* 复位次数超过 3 次就提醒一句：如果是复位导致时间老回零，病因在电源 */
            if (rst > 3u)
            {
                OLED_Print(56, UI_BOT_Y, "PWR?", 1);
            }
        }
    }

    OLED_GRAM_Refresh();
}

void UI_ClockShow(const RTC_TimeType *Time, uint8_t Heart)
{
    UI_Render(Time, UI_MODE_NORMAL, Heart);
}

void UI_ClockShowStalled(const RTC_TimeType *Time, uint8_t Heart)
{
    UI_Render(Time, UI_MODE_WARN, Heart);
}

void UI_ClockShowHeal(const RTC_TimeType *Time, uint8_t Heart)
{
    UI_Render(Time, UI_MODE_HEAL, Heart);
}

/*------------------------------------------------------------------------------
 * 排障页：直接看 RTC 原始寄存器，不用猜
 *
 *   BDCR  bit0 LSEON  bit1 LSERDY  bit9:8 RTCSEL  bit15 RTCEN  bit16 BDRST
 *   CSR   bit0 LSION  bit1 LSIRDY
 *   DIV   只读的预分频计数器，每个 RTCCLK 减 1（唯一的“时钟活着”证据）
 *   PRL   软件写进去的目标预分频值（PRLH/PRLL 只写，读不回来，只能记账）
 *
 *  ⚠ 这一页必须**持续刷新**（main 里每 400ms 调一次），盯着哪一行在动：
 *      · DIV 在动、CNT 不动 -> RTCCLK 进来了，但 PRL 太大/没写进去，
 *                              DIV 减不到 0，CNT 永远不进位（最常见）
 *      · DIV 不动           -> RTCCLK 是空的，看 SEL / EN / CSR 的 LSION
 *      · DIV、CNT 都在动    -> RTC 完全正常，“时间不动”是软件读错地方了
 *      · 连这一页都定格     -> 程序已经死了（HardFault），根本没到 I2C 那一步
 *----------------------------------------------------------------------------*/
void UI_ClockShowDiag(const RTC_DiagType *D)
{
    char buf[40];

    if (D == (RTC_DiagType *)0) { return; }

    OLED_Clear();

    /* BDCR 里 RTCSEL[9:8] 的编码：01=LSE、10=LSI、11=HSE/128、00=没时钟 */
    {
        const char *sel;
        uint32_t    selBits = (D->BDCR & 0x0300u) >> 8u;

        switch (selBits)
        {
            case 1u: sel = "LSE"; break;
            case 2u: sel = "LSI"; break;
            case 3u: sel = "HSE"; break;
            default: sel = "NONE"; break;      /* 00 = RTCCLK 是空的，秒计数器必然不动 */
        }

        snprintf(buf, sizeof(buf), "BDCR=%08lX", (unsigned long)D->BDCR);
        OLED_Print(0, 2, buf, 1);

        snprintf(buf, sizeof(buf), "CSR =%08lX", (unsigned long)D->CSR);
        OLED_Print(0, 12, buf, 1);

        snprintf(buf, sizeof(buf), "CNT =%08lX", (unsigned long)D->Counter);
        OLED_Print(0, 22, buf, 1);

        /* 复位次数：一直涨就是 MCU 在反复复位，跟 RTC 没关系。 */
        snprintf(buf, sizeof(buf), "R=%lu", (unsigned long)D->ResetCount);
        OLED_Print(88, 22, buf, 1);

        /* ⚠ 这一行是整页最有价值的：DIV 是只读的预分频计数器，
         *   每个 RTCCLK 周期减 1，减到 0 回卷并让 CNT +1。
         *   它在变 = 时钟真的进到了 RTC 内核；它不动 = RTCCLK 是空的。 */
        snprintf(buf, sizeof(buf), "DIV =%08lX", (unsigned long)D->Divider);
        OLED_Print(0, 32, buf, 1);

        /* PRL 是只写寄存器读不回来，这里显示的是软件记账的目标值；
         * CRL 看 RTOFF(bit5)/CNF(bit4)/RSF(bit3)。 */
        snprintf(buf, sizeof(buf), "PRL=%lu", (unsigned long)D->PrlTarget);
        OLED_Print(0, 42, buf, 1);

        snprintf(buf, sizeof(buf), "CRL=%04X", (unsigned)D->CRL);
        OLED_Print(62, 42, buf, 1);

        snprintf(buf, sizeof(buf), "SEL=%s EN=%u DBP=%u",
                 sel,
                 (unsigned)((D->BDCR & 0x8000u) ? 1u : 0u),
                 (unsigned)((D->PWR_CR & 0x0100u) ? 1u : 0u));
        OLED_Print(0, 52, buf, 1);
    }

    OLED_GRAM_Refresh();
}
