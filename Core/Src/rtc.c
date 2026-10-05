/*==============================================================================
 * rtc.c —— STM32F103 内部 RTC（直接寄存器操作，不依赖 HAL_RTC 模块）
 *
 * 为什么不用 HAL_RTC_*：
 *   部分 STM32F1 HAL 版本（老版本 / CubeMX 精简中间件）的 stm32f1xx_hal_rtc.h
 *   里没有 RTC_HandleTypeDef / RTC_TimeTypeDef，编译会报
 *   "unknown type name 'RTC_HandleTypeDef'"。这里直接读写 RTC 寄存器：
 *     CNT  —— 秒计数器（向上计数，计满 2^32 秒 ≈ 2136 年才溢出）
 *     PRL  —— 预分频，输出频率 = RTCCLK / (PRL + 1)。
 *             LSE：32767 -> 32768/32768 = 1 秒/次（正好）
 *             LSI：按 rtc.h 的 RTC_LSI_FREQ_HZ 算，40kHz -> 39999
 *   F1 的 RTC 只有“一个 32 位秒计数器”，没有独立的时分秒寄存器，
 *   所以这里做了 秒计数器 <-> 公历日期 的换算。
 *
 * 时间基准：2000-01-01 00:00:00 = 计数器 0
 * 时钟源：  LSE(32.768kHz) 优先；起不来就自动退回 LSI（RTC_ForceLSI 还会兜底一次）
 *============================================================================*/
#include "rtc.h"
#include "stm32f1xx_hal.h"

/*------------------------------------------------------------------------------
 * 内部辅助
 *
 * 位定义依据 RM0008：
 *   RCC_BDCR : bit0 LSEON, bit1 LSERDY, bits9:8 RTCSEL[1:0],
 *              bit15 RTCEN, bit16 BDRST
 *              RTCSEL[1:0] = 01 -> LSE, 10 -> LSI, 11 -> HSE/128（00 = 无时钟！）
 *   RCC_CSR  : bit0 LSION, bit1 LSIRDY      （F1 的 LSI 使能位在这里，不是 BDCR）
 *   PWR_CR   : bit8 DBP（后备域写允许）
 *   RCC_APB1ENR : bit0 RTCEN（RTC 寄存器在 APB1 上的接口时钟门控）
 *   RTC_CRL  : bit5 RTOFF, bit4 CNF, bit3 RSF
 *   ⚠ 没有 RTC_WPR —— F1 靠 PWR_CR.DBP 解后备域写保护，见 RTC_EnableBackupWrite()
 * （老的 CMSIS 头文件可能缺这些名字，缺的这里补上，值以手册为准）
 *----------------------------------------------------------------------------*/
#ifndef RCC_BDCR_LSEON
#define RCC_BDCR_LSEON       ((uint32_t)0x00000001)   /* bit0 外部 32.768k 使能 */
#endif
#ifndef RCC_BDCR_LSERDY
#define RCC_BDCR_LSERDY      ((uint32_t)0x00000002)   /* bit1 外部 32.768k 就绪 */
#endif
#ifndef RCC_CSR_LSION
#define RCC_CSR_LSION        ((uint32_t)0x00000001)   /* bit0 内部低速 RC 使能 */
#endif
#ifndef RCC_CSR_LSIRDY
#define RCC_CSR_LSIRDY       ((uint32_t)0x00000002)   /* bit1 内部低速 RC 就绪 */
#endif
#ifndef RCC_BDCR_RTCSEL
#define RCC_BDCR_RTCSEL      ((uint32_t)0x00000300)   /* RTCSEL[9:8] 掩码 */
#endif
#ifndef RCC_BDCR_RTCSEL_0
#define RCC_BDCR_RTCSEL_0    ((uint32_t)0x00000100)   /* 01 = LSE */
#endif
#ifndef RCC_BDCR_RTCSEL_1
#define RCC_BDCR_RTCSEL_1    ((uint32_t)0x00000200)   /* 10 = LSI */
#endif
#ifndef RCC_BDCR_RTCEN
#define RCC_BDCR_RTCEN       ((uint32_t)0x00008000)   /* bit15 RTC 内核时钟使能 */
#endif
#ifndef RCC_APB1ENR_RTCEN
#define RCC_APB1ENR_RTCEN    ((uint32_t)0x00000001)   /* bit0 RTC 寄存器接口时钟 */
#endif
#ifndef RCC_BDCR_BDRST
#define RCC_BDCR_BDRST       ((uint32_t)0x00010000)   /* bit16 后备域软件复位 */
#endif
#ifndef PWR_CR_DBP
#define PWR_CR_DBP           ((uint32_t)0x00000100)   /* bit8 后备域写允许 */
#endif
#ifndef RTC_CRL_CNF
#define RTC_CRL_CNF          ((uint16_t)0x0010)       /* bit4 配置模式 */
#endif
#ifndef RTC_CRL_RSF
#define RTC_CRL_RSF          ((uint16_t)0x0008)       /* bit3 寄存器同步标志 */
#endif
#ifndef RTC_CRL_RTOFF
#define RTC_CRL_RTOFF        ((uint16_t)0x0020)       /* bit5 上次写操作完成 */
#endif

/*------------------------------------------------------------------------------
 * 独立于头文件成员名的寄存器访问
 *
 * 不同来源的 stm32f103xx.h / stm32f10x.h 成员名并不统一，实测踩过这些坑：
 *   · BKP_TypeDef 的成员：BKP0R..BKP11R、BKP1R..BKP10R 都见过，甚至有版本
 *     根本没定义 BKP1R（于是 "BKP_TypeDef has no member named 'BKP1R'"）
 *   · RTC 预分频低 16 位：官方 CMSIS 里叫 PRLL，也有头文件写成 PRL
 * 所以这里一律用「外设基址 + 寄存器偏移」访问，偏移取自 RM0008 的寄存器映射表
 * （RTC 见 Table 95，BKP 见 Table 13），具体表格写在下面两个宏定义处。
 *
 *  ⚠ F1 **没有 RTC_WPR 寄存器**。那个"写 0xCA 再写 0x53 解锁"的流程是
 *    F2/F3/F4/L1 的机制，照搬到 F1 上就是往 0x10（只读的 DIVH）写字节，
 *    完全无效。F1 的后备域写保护只看 **PWR_CR 的 DBP(bit8)**，
 *    见下面的 RTC_EnableBackupWrite()。
 * 这样不管用 Cube 的 CMSIS、Keil 器件包还是老版 StdPeriph 的头文件都能编过。
 *----------------------------------------------------------------------------*/
#ifndef RTC_BASE
#define RTC_BASE             ((uint32_t)0x40002800UL)   /* RTC 外设基址 */
#endif
#ifndef BKP_BASE
#define BKP_BASE             ((uint32_t)0x40006C00UL)   /* BKP 外设基址 */
#endif

#define RTC_REG16(off)       (*(volatile uint16_t *)(RTC_BASE + ((uint32_t)(off))))

/* ⚠⚠ 下面这张偏移表取自 **RM0008 Table 95「RTC register map」**（16 位可寻址），
 *    并与 ST 官方 CMSIS 的 RTC_TypeDef 成员顺序、svd2rust 生成的 PAC 三处核对一致。
 *
 *    偏移   寄存器      属性
 *    0x00   RTC_CRH     读写   控制高位（SECIE/ALRIE/OWIE）
 *    0x04   RTC_CRL     读写   RTOFF(bit5) / CNF(bit4) / RSF(bit3)
 *    0x08   RTC_PRLH    只写   预分频装载高，仅 PRL[19:16] 有效
 *    0x0C   RTC_PRLL    只写   预分频装载低，PRL[15:0]
 *    0x10   RTC_DIVH    只读   预分频计数器高，实时递减
 *    0x14   RTC_DIVL    只读   预分频计数器低，实时递减
 *    0x18   RTC_CNTH    读写   秒计数器高 16 位
 *    0x1C   RTC_CNTL    读写   秒计数器低 16 位（读 CNT 必须**先读 CNTL 再读 CNTH**）
 *    0x20   RTC_ALRH    读写   闹钟高（复位值 0xFFFF）
 *    0x24   RTC_ALRL    读写   闹钟低（复位值 0xFFFF）
 *
 *  这张表曾经整张写错过，代价是排查了 4 轮。写错时**不报错也不死机**，
 *  只是现象极具迷惑性：把 CNTH/CNTL 当 0x00/0x04 去读，实际读到 CRH/CRL
 *  （恒定值）→ "时间一动不动"；把 PRLH 当 0x1C 去读，实际读到 CNTL
 *  （真秒计数器）→ "PRL 一直在变"。两个现象放一起看就是：
 *  **硬件明明在跑，软件却坚称 RTC 死了。**
 *  教训：一旦发现"某个只读/只写的寄存器自己在变"，第一反应应该是查偏移表。 */
#define RTC_R_CRH            RTC_REG16(0x00)            /* RTC_CRH  */
#define RTC_R_CRL            RTC_REG16(0x04)            /* RTC_CRL  */
#define RTC_R_PRLH           RTC_REG16(0x08)            /* RTC_PRLH，只写 */
#define RTC_R_PRLL           RTC_REG16(0x0C)            /* RTC_PRLL，只写 */
#define RTC_R_DIVH           RTC_REG16(0x10)            /* RTC_DIVH，只读，实时递减 */
#define RTC_R_DIVL           RTC_REG16(0x14)            /* RTC_DIVL，只读，实时递减 */
#define RTC_R_CNTH           RTC_REG16(0x18)            /* RTC_CNTH */
#define RTC_R_CNTL           RTC_REG16(0x1C)            /* RTC_CNTL */
#define RTC_R_ALRH           RTC_REG16(0x20)            /* RTC_ALRH */
#define RTC_R_ALRL           RTC_REG16(0x24)            /* RTC_ALRL */

/*------------------------------------------------------------------------------
 * 备份数据寄存器
 *
 * ⚠⚠ 这块连续踩过两次坑，务必看清楚（RM0008 Table 13「BKP register map」，
 *    并与标准外设库 stm32f10x_bkp.h 的 BKP_DR1 = 0x0004 核对一致）：
 *
 *      0x00  Reserved
 *      0x04  BKP_DR1      ← 第一个数据寄存器，不是 0x00
 *      0x08  BKP_DR2
 *      0x0C  BKP_DR3      …… 依次 +4，直到
 *      0x28  BKP_DR10
 *      0x2C  BKP_RTCCR    0x30 BKP_CR    0x34 BKP_CSR
 *      （高密度型号还有 DR11~DR42，从 0x40 起）
 *
 *    每个寄存器只有**低 16 位有效**，但占用的字地址间隔是 **4 字节**
 *    （高半字保留）。所以步进必须是 4，写成 2 就会落到保留半字上，
 *    写进去读不出来 —— 效果和写错地址一模一样。
 *
 *    另外别照抄 F4/L4/H7 的 "BKP0R…从 +0x50 起" —— 那是别的系列。
 *    写错地址的后果：读回来恒为 0 → "首次上电标记"永远打不上 →
 *    每次上电都把秒计数器重置回 rtc.h 的初值，
 *    看上去就是"时间每次都从头开始走 / 走两下又回到原处"。
 *----------------------------------------------------------------------------*/
#define BKP_DRx(i)           (*(volatile uint16_t *)(BKP_BASE + 0x04UL + 4UL * ((uint32_t)(i))))

#define RTC_BKP_FLAG         0xA5A5u      /* BKP_DR1 里的“已初始化”标记 */

/* 最后一次写进 PRL 的目标值。
 * ⚠ F1 的 RTC_PRLH/PRLL 是**只写**寄存器（RM0008 里标注 w），写进去读不回来，
 *   读那个地址得到的是未定义值。所以"预分频到底设成了多少"只能软件自己记一份，
 *   排障页显示 PRL 时用它。想看预分频器是不是真的在跑，要看只读的 DIV。 */
static uint32_t s_PrlTarget = 0u;

/*------------------------------------------------------------------------------
 * 底层：同步 / 配置模式 / 写保护
 *----------------------------------------------------------------------------*/
/* 等上一次 RTC 操作结束（RTOFF = 1 表示空闲）。
 * 注意：如果 RTC 压根没有时钟（RTCCLK 是空的），RTOFF 永远不会置位，
 * 所以必须带超时，否则会在这里死等。 */
static void RTC_WaitRTOFF(void)
{
    volatile uint32_t timeout = 0xFFFFu;

    while (((RTC_R_CRL & RTC_CRL_RTOFF) == 0u) && (timeout > 0u))
    {
        timeout--;
    }
}

/* 清 RSF 后等待寄存器同步完成（刚打开 APB1/RTC 时钟、或退出配置模式后必须做一次） */
static void RTC_WaitSynchro(void)
{
    volatile uint32_t timeout = 0xFFFFu;

    RTC_R_CRL = (uint16_t)(RTC_R_CRL & (uint16_t)~RTC_CRL_RSF);
    while (((RTC_R_CRL & RTC_CRL_RSF) == 0u) && (timeout > 0u))
    {
        timeout--;
    }
}

/* 进入配置模式（CNF=1）：之后才能写 PRL / CNT */
static void RTC_EnterConfigMode(void)
{
    RTC_WaitRTOFF();
    RTC_R_CRL = (uint16_t)(RTC_R_CRL | RTC_CRL_CNF);
}

/* 退出配置模式 */
static void RTC_ExitConfigMode(void)
{
    RTC_R_CRL = (uint16_t)(RTC_R_CRL & (uint16_t)~RTC_CRL_CNF);
    RTC_WaitRTOFF();
}

/* ⚠ F1 **没有 RTC_WPR 寄存器**。"写 0xCA 再写 0x53 解锁"是 F2/F3/F4/L1 的机制，
 *   照搬到 F1 上就是往偏移 0x10（只读的 RTC_DIVH）写字节 —— 完全无效。
 *
 *   F1 想写 RTC / BKP，只需要满足两个条件：
 *     1) RCC_APB1ENR 里打开 RTCEN(bit0) / PWREN(bit28) / BKPEN(bit27) 三个时钟
 *     2) PWR_CR 的 DBP(bit8) 置 1 —— 后备域写允许
 *   之后进配置模式（CRL.CNF=1）就能写 CNT 和 PRL 了。
 *
 *   后备域被 BDRST 复位后 DBP 不会被清（它在 PWR 里，不在后备域），
 *   但保险起见每次要写 RTC 前都调一次，反正都是幂等的置位操作。 */
static void RTC_EnableBackupWrite(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_RTCEN | RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN;
    PWR->CR |= PWR_CR_DBP;
}

/* 占位：F1 没有 RTC 写保护寄存器可上锁。
 * 这里**故意什么都不做** —— DBP 要保持置位，否则后面 BKP 的标记也写不进去。 */
static void RTC_LockWPR(void)
{
    /* no-op: F1 无 WPR 寄存器；DBP 保持 1 */
}

/* 读秒计数器。
 * ⚠ RM0008 要求：**必须先读 CNTL 再读 CNTH**（读 CNTL 会把 CNTH 锁存住）。
 *   顺序反了就有可能读到"低字节刚进位、高字节还没进位"拼出来的错误值。
 *   另外下面再比一次 CNTH，若两次不一致说明读的过程中又进了一位，重读。 */
static uint32_t RTC_ReadCounter(void)
{
    uint16_t cntl = (uint16_t)RTC_R_CNTL;
    uint16_t cnth = (uint16_t)RTC_R_CNTH;

    if (cnth != (uint16_t)RTC_R_CNTH)
    {
        cntl = (uint16_t)RTC_R_CNTL;
        cnth = (uint16_t)RTC_R_CNTH;
    }
    return (((uint32_t)cnth << 16) | (uint32_t)cntl);
}

/* 写预分频：PRL 是 20 位，PRLH 只有低 4 位（PRL[19:16]）有效，PRLL 是 PRL[15:0]。
 * 必须先写 PRLH 再写 PRLL，且要在配置模式（CNF=1）里写。 */
static void RTC_WritePrescaler(uint32_t Prescaler)
{
    s_PrlTarget = Prescaler;

    RTC_EnterConfigMode();
    RTC_R_PRLH = (uint16_t)((Prescaler >> 16) & 0x000Fu);
    RTC_R_PRLL = (uint16_t)(Prescaler & 0xFFFFu);
    RTC_ExitConfigMode();

    RTC_WaitSynchro();      /* 退出配置模式会清 RSF，重新同步一次再读 CNT 才准 */
}

/* 写秒计数器（CNTH 先写再写 CNTL；自动解锁/上锁写保护） */
static void RTC_WriteCounter(uint32_t Sec)
{
    RTC_EnableBackupWrite();
    RTC_EnterConfigMode();
    RTC_R_CNTH = (uint16_t)(Sec >> 16);
    RTC_R_CNTL = (uint16_t)(Sec & 0xFFFFu);
    RTC_ExitConfigMode();
    RTC_LockWPR();
}

/*------------------------------------------------------------------------------
 * 公历换算（闰年累加法：年份范围 2000~2199，直观且易于验证）
 *----------------------------------------------------------------------------*/
static const uint8_t RTC_DayOfMonth[12] =
{
    31u, 28u, 31u, 30u, 31u, 30u, 31u, 31u, 30u, 31u, 30u, 31u
};

/* 是否闰年（传入绝对年份，如 2024） */
static uint8_t RTC_IsLeapYear(int32_t Year)
{
    return (uint8_t)((((Year % 4) == 0) && ((Year % 100) != 0)) || ((Year % 400) == 0));
}

/* 该年有多少天 */
static uint16_t RTC_DaysOfYear(int32_t Year)
{
    return (uint16_t)(365u + (RTC_IsLeapYear(Year) ? 1u : 0u));
}

/* 该月有多少天 */
static uint8_t RTC_DaysInMonth(int32_t Year, uint8_t Month)
{
    if ((Month == 2u) && RTC_IsLeapYear(Year)) { return 29u; }
    return RTC_DayOfMonth[Month - 1u];
}

/* 自当年 1 月 1 日起，Month 之前累计的天数 */
static uint16_t RTC_DaysBeforeMonth(int32_t Year, uint8_t Month)
{
    uint16_t days = 0u;
    uint8_t  i;

    for (i = 1u; i < Month; i++)
    {
        days += RTC_DaysInMonth(Year, i);
    }
    return days;
}

/* 公历日期 -> 距 2000-01-01 的天数（2000-01-01 本身为 0） */
static uint32_t RTC_DaysSince2000(int32_t Year, uint8_t Month, uint8_t Day)
{
    uint32_t days = 0u;
    int32_t  y;

    for (y = 2000; y < Year; y++)
    {
        days += RTC_DaysOfYear(y);
    }
    days += RTC_DaysBeforeMonth(Year, Month);
    days += (uint32_t)(Day - 1u);

    return days;
}

/* 距 2000-01-01 的天数 -> 公历年月日 */
static void RTC_CivilFromDays(uint32_t Days, uint8_t *pYear, uint8_t *pMonth, uint8_t *pDate)
{
    int32_t  y = 2000;
    uint32_t rest = Days;

    /* 逐年推进（从 2000 起到目标年最多 200 次循环，可忽略） */
    while (rest >= RTC_DaysOfYear(y))
    {
        rest -= RTC_DaysOfYear(y);
        y++;
    }

    {
        uint8_t m;

        for (m = 1u; m <= 12u; m++)
        {
            uint8_t len = RTC_DaysInMonth(y, m);

            if (rest < (uint32_t)len)
            {
                if (pYear)  { *pYear  = (uint8_t)(y - 2000); }
                if (pMonth) { *pMonth = m; }
                if (pDate)  { *pDate  = (uint8_t)(rest + 1u); }
                return;
            }
            rest -= (uint32_t)len;
        }
    }
}

/* 日期时间 -> 秒（距 2000-01-01 00:00:00） */
static uint32_t RTC_DateTimeToSecond(const RTC_TimeType *Time)
{
    uint32_t days = RTC_DaysSince2000((int32_t)(2000 + Time->Year),
                                       Time->Month,
                                       Time->Date);

    return (days * 86400u) + ((uint32_t)Time->Hour * 3600u) +
           ((uint32_t)Time->Minute * 60u) + (uint32_t)Time->Second;
}

/* 秒 -> 日期时间 */
static void RTC_SecondToDateTime(uint32_t Sec, RTC_TimeType *Time)
{
    uint32_t days = Sec / 86400u;
    uint32_t rest = Sec % 86400u;

    RTC_CivilFromDays(days, &Time->Year, &Time->Month, &Time->Date);

    Time->Hour   = (uint8_t)(rest / 3600u);
    Time->Minute = (uint8_t)((rest % 3600u) / 60u);
    Time->Second = (uint8_t)(rest % 60u);

    /* 0 = 星期日（2000-01-01 是星期六，故 +6 后取模） */
    Time->Week = (uint8_t)((days + 6u) % 7u);
}

/*------------------------------------------------------------------------------
 * 时钟源
 *----------------------------------------------------------------------------*/
/* 打开指定振荡器并等它稳定（ms 超时，绝不死等）。返回 1 表示稳定了。
 * 注意：LSE 在 BDCR，LSI 在 CSR —— 两个不同的寄存器，别搞混。 */
static uint8_t RTC_PrepareOscillator(uint8_t UseLse)
{
    uint32_t t0 = HAL_GetTick();

    if (UseLse != 0u)
    {
        RCC->BDCR |= RCC_BDCR_LSEON;
        while (((RCC->BDCR & RCC_BDCR_LSERDY) == 0u) &&
               ((HAL_GetTick() - t0) < RTC_LSE_STARTUP_MS))
        {
            /* 等 32.768k 起振（典型几十~几百毫秒） */
        }
        return ((RCC->BDCR & RCC_BDCR_LSERDY) != 0u) ? 1u : 0u;
    }

    RCC->CSR |= RCC_CSR_LSION;
    t0 = HAL_GetTick();
    while (((RCC->CSR & RCC_CSR_LSIRDY) == 0u) && ((HAL_GetTick() - t0) < 300u))
    {
        /* 等内部 RC 稳定（通常 <1ms） */
    }
    return 1u;                                   /* LSI 是片内 RC，一定会有 */
}

/* 切换 RTC 时钟源，回读校验；不成功就再试（最多 3 次）。
 *
 * ⚠ F1 换 RTCSEL 之前必须先停 RTC（清 BDCR 的 RTCEN），而且写入后必须
 *   回读确认：BDRST（后备域复位）是异步生效的，BDCR 也会被它清回默认，
 *   所以复位之后要再写一次 RTCSEL，写完读回来比对，没写进去就重试。
 *   切换成功后再开 RTCEN。 */
static uint8_t RTC_SelectRTCClock(uint32_t WantSel)
{
    uint32_t tries;

    for (tries = 0u; tries < 3u; tries++)
    {
        if ((RCC->BDCR & RCC_BDCR_RTCSEL) == WantSel)
        {
            break;                              /* 已经是目标时钟源，一个字都不用动 */
        }

        RCC->BDCR &= ~RCC_BDCR_RTCEN;           /* 写 RTCSEL 前必须先停 RTC */

        RCC->BDCR &= ~RCC_BDCR_RTCSEL;
        RCC->BDCR |=  WantSel;
        RCC->BDCR |=  RCC_BDCR_BDRST;           /* 后备域复位（换时钟源必经） */
        RCC->BDCR &= ~RCC_BDCR_BDRST;           /* 自清位，这里手动再清一次保险 */
        RCC->BDCR &= ~RCC_BDCR_RTCSEL;
        RCC->BDCR |=  WantSel;                  /* 复位后必须重写一遍 */

        {                                       /* 等几个周期让 BDRST 释放完再确认 */
            volatile uint32_t d = 200u;
            while (d > 0u) { d--; }
        }

        if ((RCC->BDCR & RCC_BDCR_RTCSEL) == WantSel)
        {
            RCC->BDCR |= RCC_BDCR_RTCEN;        /* 时钟源定好后再开 RTC */
            return 1u;
        }
    }

    /* 实在切不过去，至少把 RTCEN 置上（RTC 时钟源暂时是旧值） */
    RCC->BDCR |= RCC_BDCR_RTCEN;
    return ((RCC->BDCR & RCC_BDCR_RTCSEL) == WantSel) ? 1u : 0u;
}

/* 选时钟源、返回预分频值（有晶振就走 LSE，否则 LSI） */
/* 后备域里累计一次“复位/重配”次数（掉电保留，存 BKP_DR2）。
 * 屏幕把它打出来：数字一直在涨就说明 MCU 在反复复位
 * （典型是电源带载不够：OLED 全屏点亮瞬间把 3.3V 拉塌了），
 * 这时候“时间不动”根本不是 RTC 的问题，跟 RTC 完全无关。 */
static void RTC_BumpResetCount(void)
{
    BKP_DRx(1) = (uint16_t)((uint32_t)BKP_DRx(1) + 1u);
}

static uint16_t RTC_ClockSourceConfig(uint8_t *pUseLse)
{
    uint16_t prescaler = (uint16_t)(RTC_LSI_FREQ_HZ - 1u);  /* 默认 LSI */
    uint8_t  useLse = 0u;

#if RTC_USE_LSE
    useLse = RTC_PrepareOscillator(1u);         /* 起振成功返回 1 */
#endif

    if (useLse == 0u)
    {
        /* LSE 起不来：关掉它（省电，也避免下次误判），改用 LSI。
         * LSI 的 PRL 要按「实际时钟频率 / 1Hz」来算：
         *   F1 只有一个预分频，输出 = RTCCLK / (PRL+1)，40kHz 想凑 1Hz 就写 39999。 */
#if RTC_USE_LSE
        RCC->BDCR &= ~RCC_BDCR_LSEON;
#endif
        (void)RTC_PrepareOscillator(0u);
        prescaler = (uint16_t)(RTC_LSI_FREQ_HZ - 1u);
    }
    else
    {
        prescaler = 32767u;                     /* 32768/(32767+1) = 1Hz */
    }

    (void)RTC_SelectRTCClock((useLse != 0u) ? RCC_BDCR_RTCSEL_0 : RCC_BDCR_RTCSEL_1);

    /* ⚠ 后备域复位（BDRST）会把 LSEON 一起清掉！
     *   所以时钟源选完之后必须再确认一次振荡器还开着 —— 否则会出现
     *   RTCSEL=01（选了 LSE）但 LSEON=0（晶振没振）的死状态，
     *   RTCCLK 是空的，秒计数器一动不动，且不报错。 */
    (void)RTC_PrepareOscillator(useLse);

    /* BDRST 会把 RTCEN 也清掉，这里无条件再置一次 */
    RCC->BDCR |= RCC_BDCR_RTCEN;

    RTC_BumpResetCount();       /* 必须放在所有 BDRST 之后，否则会被一起清掉 */

    *pUseLse = useLse;
    return prescaler;
}

/*------------------------------------------------------------------------------
 * RTC 初始化：打开后备域、选时钟源、解锁写保护、首次上电写初始时间
 *----------------------------------------------------------------------------*/
void RTC_Init(void)
{
    RTC_TimeType initTime = {0};
    uint16_t     prescaler;
    uint8_t      useLse = 0u;

    /* 1. 打开时钟并允许访问后备域。
     *    ⚠ 两个容易漏的使能位，漏一个 RTC 就「不走 / 读不到」：
     *      · RCC_APB1ENR 的 bit0 (RTCEN)：RTC 寄存器在 APB1 上的接口时钟门控。
     *        上电复位值是 0，不开的话读写 RTC 的 CNTH/CNTL 一律返回 0。
     *      · PWR_CR 的 DBP：后备域（RTC/BKP）的写允许。 */
    RCC->APB1ENR |= RCC_APB1ENR_RTCEN | RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN;
    PWR->CR |= PWR_CR_DBP;

    /* 2. 时钟源（返回预分频值）。
     *    ⚠ 复位计数的递增放在 RTC_ClockSourceConfig 里而不是这里：
     *     切时钟源要用的后备域软件复位（BDRST）会把 BKP 数据寄存器**一起清掉**，
     *     写在它前面等于白写，计数永远从 0 开始。 */
    prescaler = RTC_ClockSourceConfig(&useLse);

    /* 3. 刚开完 APB1/RTC 时钟，接口要和 RTC 内核同步一次 */
    RTC_WaitSynchro();

    /* 4. ⚠ 打开后备域写允许（PWR_CR.DBP）。F1 没有 RTC_WPR，
     *      唯一挡住写的就是 DBP 这一位：不置位的话下面对 PRL / CNT 的写
     *      全部被静默忽略，表现是"时间永远是 2000-01-01 00:00:00 且完全不动"。 */
    RTC_EnableBackupWrite();

    /* 5. 预分频：每次上电都写一遍（时钟源每次上电可能不同，
     *    PRL 必须跟当前时钟源匹配，不能只在首次上电写一次）。 */
    RTC_WritePrescaler(prescaler);

    /* 6. 秒计数器：只在首次上电写初值（BKP0R 做标记，之后复位不覆盖）。
     *    写进去会立即回读校验，没生效就不打标记，下回上电继续试。 */
    if (BKP_DRx(0) != RTC_BKP_FLAG)
    {
        uint32_t sec;

        initTime.Year   = RTC_INIT_YEAR;
        initTime.Month  = RTC_INIT_MONTH;
        initTime.Date   = RTC_INIT_DATE;
        initTime.Week   = RTC_INIT_WEEK;
        initTime.Hour   = RTC_INIT_HOUR;
        initTime.Minute = RTC_INIT_MINUTE;
        initTime.Second = RTC_INIT_SECOND;

        sec = RTC_DateTimeToSecond(&initTime);

        RTC_WriteCounter(sec);

        /* CNT 要 1~2 个 RTCCLK 后才真正生效，同步完再回读校验 */
        RTC_WaitSynchro();

        if (RTC_ReadCounter() == sec)
        {
            BKP_DRx(0) = RTC_BKP_FLAG;     /* 计数确实写进去了，打上标记 */
        }
    }

    RTC_LockWPR();
}

/*------------------------------------------------------------------------------
 * 读当前时间（十进制，0=星期日）
 *----------------------------------------------------------------------------*/
void RTC_Get(RTC_TimeType *Time)
{
    RTC_SecondToDateTime(RTC_ReadCounter(), Time);
}

uint32_t RTC_GetCounter(void)
{
    return RTC_ReadCounter();
}

/*------------------------------------------------------------------------------
 * 手动设置时间（首次上电时间不对时，改 rtc.h 的宏重新烧录一次即可；
 * 也可以在按键/串口里调用它在线改时间）
 *----------------------------------------------------------------------------*/
void RTC_Set(RTC_TimeType *Time)
{
    uint32_t sec;

    sec = RTC_DateTimeToSecond(Time);

    /* 先重算星期，保证 Week 字段与实际日期一致 */
    Time->Week = (uint8_t)((sec / 86400u + 6u) % 7u);

    RCC->APB1ENR |= RCC_APB1ENR_RTCEN | RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN;
    PWR->CR |= PWR_CR_DBP;

    RTC_WaitSynchro();                    /* 退出配置模式写 CRL 会清 RSF，先同步 */

    RTC_WriteCounter(sec);                /* 内部自带解锁/上锁写保护 */

    RTC_WaitSynchro();                    /* 保证之后 RTC_Get() 读到的 CNT 可靠 */
}

/*------------------------------------------------------------------------------
 * 秒计数器还在往前走吗？（非阻塞版，给运行时巡检用）
 *   返回 1 = 期间涨过（正常）；返回 0 = 一直没动（RTCCLK 是空的 / 被写保护挡住）
 *  ⚠ 它靠 HAL_Delay 制造间隔，调用期间主循环会被占住，别传超过 1 秒的值。
 *----------------------------------------------------------------------------*/
uint8_t RTC_IsRunning(uint32_t TimeoutMs)
{
    uint32_t a = RTC_ReadCounter();
    uint32_t t0 = HAL_GetTick();

    while ((HAL_GetTick() - t0) < TimeoutMs)
    {
        HAL_Delay(25u);
        if (RTC_ReadCounter() != a)
        {
            return 1u;                    /* 秒在跳，RTC 正常 */
        }
    }
    return 0u;
}

uint8_t RTC_SelfTest(void)
{
    return RTC_IsRunning(RTC_SELFTEST_TIMEOUT_MS);
}

/* 当前 RTCSEL 对应的时钟源名字。
 * F1 的编码：00=无时钟, 01=LSE, 10=LSI, 11=HSE/128
 * 返回 "NONE" 就是 RTCCLK 空了，秒计数器必然一动不动，且不报任何错。 */
const char *RTC_GetSourceName(void)
{
    uint32_t sel = (uint32_t)(RCC->BDCR & RCC_BDCR_RTCSEL);

    switch (sel)
    {
        case 0x00000300UL:            return "HSE";   /* 11 = HSE/128 */
        case RCC_BDCR_RTCSEL_1:       return "LSI";   /* 10 */
        case RCC_BDCR_RTCSEL_0:       return "LSE";   /* 01 */
        default:                      return "NONE";  /* 00 */
    }
}

/* 后备域里累计的复位次数（存 BKP_DR2，掉电保留） */
uint32_t RTC_GetResetCount(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_RTCEN | RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN;
    PWR->CR |= PWR_CR_DBP;
    return (uint32_t)BKP_DRx(1);
}

/*------------------------------------------------------------------------------
 * 强制改用内部 LSI（放弃 32.768k），并把初始时间按 LSI 的预分频重写一遍。
 * 自检不过时调用：绝大多数"时间不动"都是 LSE 起不来导致的，走这条就好。
 *----------------------------------------------------------------------------*/
void RTC_ForceLSI(void)
{
    RTC_TimeType initTime = {0};
    uint32_t     sec;

    RCC->APB1ENR |= RCC_APB1ENR_RTCEN | RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN;
    PWR->CR |= PWR_CR_DBP;

    /* 0. 先摘掉“配置模式”这个雷：CNF 一旦卡在 1，秒计数器就彻底冻住、
     *    读也正常、还不报错——正是“时间走着走着突然不动了”的经典成因。
     *    平时代码每处写 CNT/PRL 都有 Enter/Exit 配对，这里再兜一次底。 */
    if ((RTC_R_CRL & RTC_CRL_CNF) != 0u)
    {
        RTC_ExitConfigMode();
    }

#if RTC_USE_LSE
    /* 先把不工作的晶振关掉——不然下次上电 LSE 又白等 1 秒 */
    RCC->BDCR &= ~RCC_BDCR_LSEON;
#endif

    (void)RTC_PrepareOscillator(0u);                 /* 开 LSI */
    (void)RTC_SelectRTCClock(RCC_BDCR_RTCSEL_1);     /* RTCSEL = 10 (LSI) */
    RTC_WaitSynchro();
    RTC_BumpResetCount();                            /* 同上，放在 BDRST 之后 */

    /* 写完时钟源再确认一次后备域可写（BDRST 不改 PWR，但保险起见） */
    RTC_EnableBackupWrite();

    RTC_WritePrescaler((uint16_t)(RTC_LSI_FREQ_HZ - 1u));

    /* 后备域被复位过的话 BKP 标记也没了，所以按 rtc.h 的初值再写一次时间 */
    initTime.Year   = RTC_INIT_YEAR;
    initTime.Month  = RTC_INIT_MONTH;
    initTime.Date   = RTC_INIT_DATE;
    initTime.Week   = RTC_INIT_WEEK;
    initTime.Hour   = RTC_INIT_HOUR;
    initTime.Minute = RTC_INIT_MINUTE;
    initTime.Second = RTC_INIT_SECOND;

    sec = RTC_DateTimeToSecond(&initTime);
    RTC_WriteCounter(sec);
    RTC_WaitSynchro();

    if (RTC_ReadCounter() == sec)
    {
        BKP_DRx(0) = RTC_BKP_FLAG;
    }

    RTC_LockWPR();
}

/*------------------------------------------------------------------------------
 * 抓一份原始寄存器（屏幕诊断页用）：“时间不动”时照着这几个值看就能定论
 *----------------------------------------------------------------------------*/
void RTC_GetDiag(RTC_DiagType *D)
{
    if (D == (void *)0) { return; }

    D->BDCR    = RCC->BDCR;
    D->CSR     = RCC->CSR;
    D->APB1ENR = RCC->APB1ENR;
    D->PWR_CR  = PWR->CR;
    D->CNTH    = (uint16_t)RTC_R_CNTH;
    D->CNTL    = (uint16_t)RTC_R_CNTL;

    /* 读 DIV 要按 RM0008 的顺序：先 DIVL 再 DIVH（和读 CNT 同理） */
    D->DIVL    = (uint16_t)RTC_R_DIVL;
    D->DIVH    = (uint16_t)RTC_R_DIVH;
    D->Divider = (((uint32_t)D->DIVH << 16) | (uint32_t)D->DIVL);

    /* PRL 只写读不回来，这里给的是软件记账的目标值 */
    D->PrlTarget = s_PrlTarget;

    D->CRL     = (uint16_t)RTC_R_CRL;
    D->Counter = (((uint32_t)D->CNTH << 16) | (uint32_t)D->CNTL);
    D->ResetCount = (uint32_t)BKP_DRx(1);
}
