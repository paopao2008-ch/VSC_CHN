/*==============================================================================
 * main.c —— STM32F103C8T6 + SSD1315 OLED
 *
 * 两个独立的应用模式，用下面的 APP_MARQUEE_DEMO 宏切换：
 *
 *   APP_MARQUEE_DEMO = 1（默认）中文跑马灯
 *       屏幕上下居中排两行 16x16 点阵文字，上行从左向右滑动、
 *       下行从右向左滑动，一直循环。
 *         第 1 行：静态文字 "您好，我是STM32"      （ui_marquee.h 里改）
 *         第 2 行：动态文字 "现在是HH:MM:SS"       （RTC 实时时间，每秒刷新）
 *       ⚠ 这个模式也要用 RTC，所以同样会调 RTC_Init()；RTC 不走时
 *         第 2 行会显示 "现在是--:--:--"，滚动照常，一眼看出是时钟的问题。
 *       文字/前缀/速度/行距都在 ui_marquee.h 里改。
 *
 *   APP_MARQUEE_DEMO = 0        电子钟
 *       屏幕中间大号粗体实时显示 时:分:秒，下方小号显示 年-月-日 + 星期，
 *       时钟源为 STM32 内部 RTC（首次上电由 rtc.h 里的宏初始化，掉电不走丢）。
 *
 * 接线：  OLED VCC -> 3.3V(或模块允许的 5V)   GND -> GND
 *         OLED SCL -> PB6   OLED SDA -> PB7      （在 oled_ssd1315.h 里可改）
 *
 * 新增文件：font_cn.c/.h（16x16 中文字模，由 Scripts/gen_font_cn.py 生成）
 *           ui_marquee.c/.h（两行反向滚动的跑马灯界面）
 *
 * 与 CubeMX 的关系：
 *   这份 main.c 是**独立完整**的，里面的 SystemClock_Config / MX_GPIO_Init /
 *   Error_Handler 都和 CubeMX 生成的同名函数冲突，必须用它整体替换 CubeMX 的 main.c。
 *   CubeMX 只保留这些文件就够：*.s 启动文件、system_stm32f1xx.c、stm32f1xx_it.c、
 *   stm32f1xx_hal_conf.h、stm32f1xx_hal_msp.c（不需要）、 Drivers/ 下的 HAL + CMSIS。
 *   详细勾选清单见 README 第 7 节「CubeMX 集成注意事项」。
 *============================================================================*/
#include "stm32f1xx_hal.h"

#include "oled_ssd1315.h"
#include "rtc.h"                /* 跑马灯第 2 行要显示实时时间，两个模式都要 */
#include "ui_marquee.h"

/*------------------------------------------------------------------------------
 * 应用模式选择：1 = 中文跑马灯（默认）  0 = RTC 电子钟
 * 不想改源码的话，也可以在编译选项里加 -DAPP_MARQUEE_DEMO=0 来切。
 *----------------------------------------------------------------------------*/
#ifndef APP_MARQUEE_DEMO
#define APP_MARQUEE_DEMO      1
#endif

#if (APP_MARQUEE_DEMO == 0)
#include "ui_clock.h"
#endif

static void SystemClock_Config(void);
static void MX_GPIO_Init(void);

/* 连续多少次“读到同一个秒”才判定 RTC 停了（巡检周期 1 秒，3 = 连续 3 秒没涨） */
#define MAIN_STALL_TICKS      3u

#if (APP_MARQUEE_DEMO == 0)
/* 自愈最小间隔，避免适才坏掉又立刻救护、来回抖 */
#define MAIN_HEAL_INTERVAL_MS 8000u
/* 自愈失败多少次后直接进排障页 */
#define MAIN_HEAL_MAX_FAIL    3u
#endif

/* 跑马灯主循环的轮询周期：10ms 足够跟上 40ms/像素的位移，
 * 又不会让主循环空转（UI_MarqueeShow 内部会跳过位置没变的帧） */
#define MAIN_MARQUEE_POLL_MS  10u
/* 跑马灯模式下多久重新拼一次时间文本（100ms 足够，秒变即刻上屏） */
#define MAIN_MARQUEE_TEXT_MS  100u

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    OLED_Init();         /* OLED GPIO + SSD1315 初始化 */

#if (APP_MARQUEE_DEMO != 0)
    /*==========================================================================
     * 模式 1：中文跑马灯
     *   上行"您好，我是STM32"从左向右、下行"现在是HH:MM:SS"从右向左，
     *   两行上下居中，循环滚动。
     *   第 2 行是实时时间，所以这个模式也要跑 RTC。
     *========================================================================*/
    {
    RTC_TimeType now;
    char         line2[UI_MARQUEE_LINE2_BUF];
    uint32_t     lastCnt      = 0xFFFFFFFFu;
    uint32_t     lastTextTick = 0u;
    uint32_t     lastChkTick  = 0u;
    uint8_t      stallTicks   = 0u;
    uint8_t      valid        = 1u;

    RTC_Init();                 /* 打开 PWR/BKP/APB1-RTC 时钟、使能后备域写、配置时钟源 */

    RTC_Get(&now);
    UI_MarqueeFormatTime(line2, (uint16_t)sizeof(line2),
                         now.Hour, now.Minute, now.Second, 1u);
    UI_MarqueeInit();

    while (1)
    {
        /* ---- 每 100ms 重拼一次时间文本：秒一变就上屏，不等像素步进 ---- */
        if ((HAL_GetTick() - lastTextTick) >= MAIN_MARQUEE_TEXT_MS)
        {
            lastTextTick = HAL_GetTick();
            RTC_Get(&now);
            UI_MarqueeFormatTime(line2, (uint16_t)sizeof(line2),
                                 now.Hour, now.Minute, now.Second, valid);
        }

        /* ---- 每 1 秒巡检一次秒计数器，连续 3 秒没涨就把时间显示成横杠 ---- */
        if ((HAL_GetTick() - lastChkTick) >= 1000u)
        {
            uint32_t cnt = RTC_GetCounter();

            lastChkTick = HAL_GetTick();
            if (cnt != lastCnt)
            {
                lastCnt    = cnt;
                stallTicks = 0u;
            }
            else if (stallTicks < 200u)
            {
                stallTicks++;
            }
            valid = (uint8_t)((stallTicks < MAIN_STALL_TICKS) ? 1u : 0u);
        }

        (void)UI_MarqueeShow(HAL_GetTick(), line2);   /* 位置或文字变了才重画整屏 */
        HAL_Delay(MAIN_MARQUEE_POLL_MS);
    }
    }   /* 跑马灯模式作用域结束 */
#else
    /*==========================================================================
     * 模式 0：RTC 电子钟
     *========================================================================*/
    {
    RTC_TimeType  now;
    RTC_DiagType  diag;
    uint32_t lastCnt      = 0xFFFFFFFFu;
    uint32_t lastHealTick = 0u;
    uint8_t  mode         = 0u;      /* 0=正常 1=告警 2=自愈中 3=排障页 */
    uint8_t  frame        = 0u;      /* 100ms 帧计数 */
    uint8_t  stallTicks   = 0u;      /* 连续几个 1 秒巡检周期没看到 CNT 涨 */
    uint8_t  healFail     = 0u;      /* 自愈失败次数 */
    uint8_t  heart        = 0u;      /* 心跳块：每 200ms 翻一次 */

    RTC_Init();          /* 内部 RTC（打开 PWR/BKP/APB1-RTC 时钟，使能后备域写） */

    /* 先画一次，避免上电黑屏 */
    RTC_Get(&now);
    UI_ClockShow(&now, 0u);
    lastCnt   = RTC_GetCounter();
    lastHealTick = HAL_GetTick();

    while (1)
    {
        HAL_Delay(100u);
        frame++;
        heart = (uint8_t)((frame & 1u));        /* 200ms 翻转一次，证明程序还活着 */

        /* ---------- 排障页：持续刷新，让数字自己动起来 ---------- */
        if (mode == 3u)
        {
            if ((frame % 4u) == 0u)
            {
                RTC_GetDiag(&diag);
                UI_ClockShowDiag(&diag);
            }
            continue;
        }

        /* ---------- 上电第一次巡检：先确认时钟源能用 ---------- */
        if (frame == 1u)
        {
            /* ⚠ 窗口必须 > 1 秒 + 余量：CNT 最快也就是 1 秒才 +1，
             *   窗口取 600ms 会因为采样相位刚好躲过进位而误判成"停了"。 */
            if (RTC_IsRunning(1300u) == 0u)
            {
                RTC_ForceLSI();                 /* 强制切内部 LSI 重来 */
                RTC_Get(&now);
                if (RTC_IsRunning(1300u) == 0u)
                {
                    mode = 3u;                  /* 自检两次都不行 -> 排障页 */
                }
            }
            lastCnt    = RTC_GetCounter();
            stallTicks = 0u;
            continue;
        }

        /* ---------- 读时间，每 100ms 无条件重画 ---------- */
        RTC_Get(&now);

        /* ---------- 每秒巡检一次秒计数器 ---------- */
        if ((frame % 10u) == 0u)
        {
            uint32_t cnt = RTC_GetCounter();

            if (cnt != lastCnt)
            {
                lastCnt    = cnt;
                stallTicks = 0u;
                if (mode == 1u) { mode = 0u; }   /* 已经恢复，把告警撤掉 */
            }
            else
            {
                if (stallTicks < 200u) { stallTicks++; }
            }
        }

        /* ---------- 连续 3 秒不动：告警 + 自动自愈 ---------- */
        if (stallTicks >= MAIN_STALL_TICKS)
        {
            /* 没到自愈时机就先老实画“!RTC STOP”，别一直挂着 HEALING 装忙 */
            mode = 1u;

            if ((HAL_GetTick() - lastHealTick) > MAIN_HEAL_INTERVAL_MS)
            {
                lastHealTick = HAL_GetTick();

                /* 自愈真正开跑的这一帧才显示 HEAL 提示
                 * （下面 RTC_ForceLSI + 500ms 观察期间屏幕会停在这一帧，正常） */
                UI_ClockShowHeal(&now, heart);

                RTC_ForceLSI();                 /* 切 LSI + 重设 PRL + 重写时间 */
                RTC_Get(&now);

                if (RTC_IsRunning(1300u))       /* 1.3 秒内涨了 = 救回来了 */
                {
                    stallTicks = 0u;
                    lastCnt    = RTC_GetCounter();
                    healFail   = 0u;
                    mode       = 0u;
                }
                else if ((uint8_t)(healFail + 1u) >= MAIN_HEAL_MAX_FAIL)
                {
                    mode = 3u;                  /* 三回都救不回来 -> 看寄存器 */
                }
                else
                {
                    healFail++;
                }
                continue;
            }
            /* 还没到下一次自愈时机：保持 mode=1，下面画 !RTC STOP */
        }
        else
        {
            mode = 0u;                          /* 走得好好的 */
        }

        /* 每 100ms 重画一次：画面永远是最新的。
         * 如果哪天整个画面连心跳都定格了，那就是程序死了（不是 RTC 停了）。 */
        if (mode == 1u)      { UI_ClockShowStalled(&now, heart); }
        else if (mode == 2u) { UI_ClockShowHeal(&now, heart); }
        else                 { UI_ClockShow(&now, heart); }
    }
    }   /* 时钟模式作用域结束 */
#endif
}

/*------------------------------------------------------------------------------
 * 系统时钟：HSE 8MHz -> PLL x9 -> 72MHz，APB1 = 36MHz
 *
 * ⚠ CubeMX 相关：如果下面是你自己从 CubeMX "Clock Configuration" 生成的
 *   SystemClock_Config()，请**整段替换成这份**（它多了 HSE 起不来的自动回退），
 *   并且 —— 生成后 main.c 会被 CubeMX 覆盖，所以正确流程是：
 *   CubeMX 生成完 -> 把这份 main.c 盖回去，其余文件（rtc/oled/ui/字模）不动。
 *
 * 为什么加 HSE 回退：HAL_RCC_OscConfig() 等不到 HSERDY 会返回 HAL_TIMEOUT，
 * 若按 CubeMX 默认写法 `if (...) Error_Handler()` 就直接死在 SystemInit 里，
 * 表现是**上电黑屏、OLED 一点反应都没有**，排查方向完全跑偏。
 * 这里改成自动退 HSI（4MHz x16 = 64MHz），屏幕能亮、RTC 照走。
 *----------------------------------------------------------------------------*/
#ifndef RCC_CR_HSEON
#define RCC_CR_HSEON           ((uint32_t)0x00010000)   /* bit16 HSE 使能 */
#endif
#ifndef RCC_CR_PLLON
#define RCC_CR_PLLON           ((uint32_t)0x01000000)   /* bit24 PLL 使能  */
#endif
#ifndef RCC_PLLSOURCE_HSI_DIV2
#define RCC_PLLSOURCE_HSI_DIV2 ((uint32_t)0x00000000)   /* F1: PLL 输入恒为 HSI/2 */
#endif

static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    /* F103C8T6(48脚) 的 OSC_IN/OSC_OUT 在 PD0/PD1，需要打开 GPIOD 时钟。
     * 注意别在 CubeMX 里把 PD0/PD1 当普通 GPIO 用了，会抢晶振引脚。 */
    __HAL_RCC_GPIOD_CLK_ENABLE();

    /* 第 1 路：HSE 8MHz -> PLL x9 -> 72MHz */
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState       = RCC_HSE_ON;
    /* 注意：RCC_OscInitTypeDef 的 HSE 分频字段各 HAL 版本名字不一样——
     * 新版叫 HSEPredivValue，老版叫 HSEPrediv，所以这里干脆不碰它。
     * F103C8T6 的 CFGR.PLLXTPRE 复位值就是 0（HSE 直接进 PLL，不分频），
     * 与 HSEPredivValue = RCC_HSE_PREDIV_DIV1 完全等效，省掉这行不会有任何差别。
     */
    osc.PLL.PLLState   = RCC_PLL_ON;
#ifndef RCC_PLLSOURCE_HSE
/* 兜底：CFGR bit16 —— 1 = HSE 作 PLL 输入，0 = HSI/2（拿错会跑 36MHz） */
#define RCC_PLLSOURCE_HSE   ((uint32_t)0x00010000)
#endif
    osc.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLMUL     = RCC_PLL_MUL9;          /* 8MHz x 9 = 72MHz */

    if (HAL_RCC_OscConfig(&osc) == HAL_OK)
    {
        /* HSE 起来，用 72MHz 那一档 */
    }
    else
    {
        /* 第 2 路：HSE 起不来（没焊 8M 晶振 / CubeMX 里误配成 Bypass /
         * 负载电容不对），退到内部 HSI，别黑屏死等 */
        RCC->CR &= ~RCC_CR_HSEON;              /* 先关掉没起来的 HSE */
        RCC->CR &= ~RCC_CR_PLLON;              /* 再关 PLL 才能换输入源 */

        osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
        osc.HSEState       = RCC_HSE_OFF;
        osc.HSIState       = RCC_HSI_ON;
        osc.LSEState       = RCC_LSE_OFF;
        osc.LSIState       = RCC_LSI_OFF;
        osc.PLL.PLLState   = RCC_PLL_ON;
        osc.PLL.PLLSource  = RCC_PLLSOURCE_HSI_DIV2;   /* F1 的 HSI 进 PLL 要先 /2 */
        osc.PLL.PLLMUL     = RCC_PLL_MUL16;            /* 4MHz x16 = 64MHz */
        (void)HAL_RCC_OscConfig(&osc);
    }

    clk.ClockType      = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                         RCC_CLOCKTYPE_PCLK1  | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV2;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2) != HAL_OK) { while (1) { } }
}

/*==============================================================================
 * GPIO：OLED 的 SCL/SDA 由 OLED_GPIO_Init() 负责，这里保留给用户自己扩展按键等
 *============================================================================*/
static void MX_GPIO_Init(void)
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    /* 用户在此添加自己的 GPIO 初始化（按键、LED …） */
}

/* 若使用 CubeMX，可自动生成 mxflags.h 之类的回调，这里给个空实现占位 */
void Error_Handler(void)
{
    while (1) { HAL_Delay(200u); }
}
