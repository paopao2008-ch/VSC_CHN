/*==============================================================================
 * rtc.h —— STM32F103 内部 RTC（实时时钟）驱动
 *
 * 首次上电（BKP 寄存器为默认值）时用下面宏定义的“初始时间”写一次 RTC，
 * 之后每次复位都会接着走；掉电时间不丢失（VBAT 接纽扣电池时）。
 *
 * 若你外接了 DS3231/DS1307 模块，只需要把 RTC_Get() 换成 I2C 读 0x68 那 7 个
 * 寄存器即可，其余代码不用动。
 *============================================================================*/
#ifndef __RTC_H
#define __RTC_H

#include <stdint.h>

/*------------------------------------------------------------------------------
 * 初始时间（仅首次上电写入一次）
 *   星期：0=周日 1=周一 ... 6=周六（STM32 RTC 约定）
 *----------------------------------------------------------------------------*/
#define RTC_INIT_YEAR        26      /* 年 -> 2026 的后两位，显示时自动补 "20" */
#define RTC_INIT_MONTH       10
#define RTC_INIT_DATE        4
#define RTC_INIT_WEEK        0       /* 0 = 星期日 */
#define RTC_INIT_HOUR        8
#define RTC_INIT_MINUTE      4
#define RTC_INIT_SECOND      0

/* RTC 时钟源：1 = 外部 32.768kHz 晶振(LSE)；0 = 内部低速 RC(LSI，误差较大) */
#define RTC_USE_LSE          1

/*------------------------------------------------------------------------------
 * 只有 RTC_USE_LSE = 0（用内部 LSI）时才用得上。
 *
 * F1 的 RTC 只有单一预分频，1Hz 靠 RTCCLK / (PRL+1) ≈ 1 凑出来的，
 * 所以 LSI 必须按实际频率算：PRL = 本值 - 1（40kHz -> 39999）。
 * 写个太小的数（比如 397）时间会按 PRL+1 倍速度乱跳。
 *
 * LSI 是 RC 振荡器，出厂和实际电压下常在 30~45kHz 之间飘，走时误差可能每天
 * 差好几分钟。想要准就焊 32.768kHz（把 RTC_USE_LSE 设 1）。
 * 实在要用 LSI 又想校准：把 RTC_USE_LSE 设 0、先设成 LSE 跑一会让 RTC 计时，
 * 用示波器量 PC13（BKP 可把 RTCCLK 输出到 PC13）算实际频率，再改下面这个宏。
 *----------------------------------------------------------------------------*/
#ifndef RTC_LSI_FREQ_HZ
#define RTC_LSI_FREQ_HZ      40000u
#endif

/*------------------------------------------------------------------------------
 * LSE（32.768kHz 晶振）起振等待时间（毫秒）。
 *
 * ⚠ 时间不能设太短：晶振从 LSEON 置位到 LSERDY 稳定，典型要几百毫秒，
 *   低温/负载电容偏了可能要 1 秒以上。等不到就自动退回 LSI，所以给宽松点
 *   反而是对的（宁可等 1 秒，也别一上来就误判成"没焊晶振"）。
 *----------------------------------------------------------------------------*/
#ifndef RTC_LSE_STARTUP_MS
#define RTC_LSE_STARTUP_MS   1000u
#endif

/*------------------------------------------------------------------------------
 * 上电自检时，等多久还看不到秒计数器前进就判定"RTC 没在走"
 *----------------------------------------------------------------------------*/
#ifndef RTC_SELFTEST_TIMEOUT_MS
#define RTC_SELFTEST_TIMEOUT_MS  2500u
#endif

/*------------------------------------------------------------------------------
 * 诊断信息：把 RTC 相关的原始寄存器一次读出来，供屏幕 diagnose 页显示。
 * 现场排查"时间不动"时，直接看这几个值就能定论，不用猜。
 *----------------------------------------------------------------------------*/
typedef struct
{
    uint32_t BDCR;      /* RCC_BDCR  : LSEON/LSERDY/RTCSEL[9:8]/RTCEN(bit15)/BDRST(bit16) */
    uint32_t CSR;       /* RCC_CSR   : LSION(bit0)/LSIRDY(bit1)                        */
    uint32_t APB1ENR;   /* RCC_APB1ENR: bit0=RTC 接口时钟, bit27=BKPEN, bit28=PWREN     */
    uint32_t PWR_CR;    /* PWR_CR   : bit8 = DBP（后备域写允许）                        */
    uint16_t CRL;       /* RTC_CRL   : bit5 RTOFF / bit4 CNF / bit3 RSF                 */
    uint16_t CNTH;      /* RTC_CNTH                                                     */
    uint16_t CNTL;      /* RTC_CNTL                                                     */
    uint16_t DIVH;      /* RTC_DIVH  : 预分频计数器高，**只读**，实时递减                */
    uint16_t DIVL;      /* RTC_DIVL  : 预分频计数器低，**只读**，实时递减                */
    uint32_t Divider;   /* DIVH<<16 | DIVL                                              */
                        /*  ⚠ 这是判断 RTCCLK 有没有在跑的**唯一**指标：                 */
                        /*    它每个 RTCCLK 周期减 1，减到 0 就回卷并让 CNT +1。          */
                        /*    · DIV 在变、CNT 不变  -> 预分频值太大（PRL 没写进去）       */
                        /*    · DIV 不动            -> RTCCLK 是空的（RTCSEL 或振荡器）   */
    uint32_t PrlTarget; /* 软件写进 PRL 的目标值（PRLH/PRLL 只写，读不回来，只能记账）    */
    uint32_t Counter;   /* 合成后的秒计数器 CNTH<<16 | CNTL                             */
    uint32_t ResetCount;/* 后备域里累计的复位次数（BKP_DR2），用来区分“反复复位”与“RTC 停了” */
} RTC_DiagType;

/*------------------------------------------------------------------------------
 * 时间结构体（十进制，非 BCD）
 *----------------------------------------------------------------------------*/
typedef struct
{
    uint8_t Year;       /* 00~99  */
    uint8_t Month;      /* 1~12   */
    uint8_t Date;       /* 1~31   */
    uint8_t Week;       /* 0~6    */
    uint8_t Hour;       /* 0~23   */
    uint8_t Minute;     /* 0~59   */
    uint8_t Second;     /* 0~59   */
} RTC_TimeType;

/*==============================================================================
 * 运行时诊断 / 自愈接口（“时间不动”时靠这几个立刻分清是死机还是 RTC 停了）
 *============================================================================*/

/* 非阻塞地在 ms 毫秒内观察秒计数器有没有前进。
 *  返回 1 = 期间涨过（RTC 正常）；返回 0 = 一直没动（RTC 停了）。
 *  ⚠ 它用 HAL_Delay 做间隔，所以调用期间主循环会被占住，别传太大的值（>1 秒）。 */
uint8_t     RTC_IsRunning(uint32_t TimeoutMs);

/* 自愈：强制把 RTC 时钟源切到内部 LSI，重设预分频与时间。
 *  ⚠ 切时钟源要走一次后备域复位（BDRST），**秒计数器会被清零**，
 *    所以它内部会自动用 rtc.h 的 RTC_INIT_* 把时间重新写回去，
 *    并且会同步改写“首次上电标记”——正常显示模式下不需要手动调用，
 *    主循环检测到停摆时会自动调它。 */
void        RTC_ForceLSI(void);

/* 抓一份原始寄存器状态（排障页显示用）。 */
void        RTC_GetDiag(RTC_DiagType *D);

/* 当前用的时钟源名字，给界面显示：返回 "LSE" / "LSI" / "HSE" / "NONE"
 * "NONE" 意味着 RTCSEL=00，RTCCLK 是空的，秒计数器必然一动不动。 */
const char *RTC_GetSourceName(void);

/* 后备域里累计的复位次数（上电自检计数）。
 *  ⚠ 复用 BKP_DR2 存，只增不减、掉电保留。
 *    屏幕上把它显示出来：如果数值一直在涨（比如每秒 +1），
 *    说明 MCU 在反复复位（电源带载不够 / OLED 刷新时电压跌落），
 *    而不是 RTC 停了——这两种故障的修法完全不同。 */
uint32_t    RTC_GetResetCount(void);

/* 兼容旧名字：自检期间计数器有前进返回 1 */
uint8_t     RTC_SelfTest(void);

void        RTC_Init(void);
void        RTC_Get(RTC_TimeType *Time);
void        RTC_Set(RTC_TimeType *Time);

/* 直接读秒计数器（不上日期换算），巡检“停没停”时用它最省事 */
uint32_t    RTC_GetCounter(void);

#endif /* __RTC_H */
