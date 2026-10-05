/*==============================================================================
 * oled_ssd1315.c —— SSD1315(兼容 SSD1306 指令集) 128x64 OLED 驱动
 *
 * 说明：
 *   1. 内部 GRAM：OLED_PAGE_NUM * OLED_WIDTH = 8 * 128 = 1024 字节
 *   2. 初始化时把列地址(0~127)/页地址(0~7)设成全屏窗口，
 *      之后刷新只需要发 0xB0+页号 与 128 字节数据即可
 *   3. SSD1315 与 SSD1306 指令完全兼容；若你的模块是 128x32 之类，
 *      改 OLED_HEIGHT / 初始化里的 MUX 参数即可
 *============================================================================*/
#include "oled_ssd1315.h"
#include "font5x7.h"

/*------------------------------------------------------------------------------
 * 软件 I2C 微调用（数值越大越慢，100kHz~400kHz 都能稳定工作）
 *----------------------------------------------------------------------------*/
#ifndef OLED_I2C_DELAY_NOP
#define OLED_I2C_DELAY_NOP   12u
#endif

/*------------------------------------------------------------------------------
 * SSD1315 初始化命令表（关屏 -> 基本配置 -> 开屏）
 *----------------------------------------------------------------------------*/
static const uint8_t s_InitCmd[] = {
    0xAE,                   /* 显示关闭                     */
    0xD5, 0x80,             /* 时钟分频 / 振荡频率          */
    0xA8, 0x3F,             /* 多路复用比 = 1/64            */
    0xD3, 0x00,             /* 显示偏移                     */
    0x40,                   /* 显示起始行 = 0               */
    0xA1,                   /* 段重映射（左右镜像）         */
    0xC8,                   /* COM 扫描方向（上下翻转）     */
    0xDA, 0x12,             /* COM 引脚配置（128x64 专用）  */
    0x8D, 0x14,             /* 电荷泵开启                   */
    0x81, 0x8A,             /* 对比度                       */
    0xD9, 0xF1,             /* 预充电周期                   */
    0xDB, 0x35,             /* VCOM 去选择电平              */
    0x20, 0x00,             /* 寻址模式 = 水平地址递增     */
    0xAF                    /* 显示开启                     */
};

/* 显存：按页存放，index = Page * OLED_WIDTH + X */
static uint8_t s_GRAM[OLED_PAGE_NUM * OLED_WIDTH];

/*------------------------------------------------------------------------------
 * 底层：软件 I2C
 *----------------------------------------------------------------------------*/
static void SW_I2C_Delay(void)
{
    volatile uint32_t i;
    for (i = 0u; i < OLED_I2C_DELAY_NOP; i++) { __NOP(); }
}

static void SW_I2C_SCL(uint8_t Level)
{
    HAL_GPIO_WritePin(OLED_SCL_PORT, OLED_SCL_PIN, Level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void SW_I2C_SDA(uint8_t Level)
{
    HAL_GPIO_WritePin(OLED_SDA_PORT, OLED_SDA_PIN, Level ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void SW_I2C_Start(void)
{
    SW_I2C_SDA(1u); SW_I2C_Delay();
    SW_I2C_SCL(1u); SW_I2C_Delay();
    SW_I2C_SDA(0u); SW_I2C_Delay();
    SW_I2C_SCL(0u); SW_I2C_Delay();
}

static void SW_I2C_Stop(void)
{
    SW_I2C_SDA(0u); SW_I2C_Delay();
    SW_I2C_SCL(1u); SW_I2C_Delay();
    SW_I2C_SDA(1u); SW_I2C_Delay();
}

/* 发送 1 字节。OLED 一般不给 ACK，这里直接忽略应答位 */
static void SW_I2C_SendByte(uint8_t Byte)
{
    uint8_t i;
    for (i = 0u; i < 8u; i++)
    {
        SW_I2C_SDA((Byte & 0x80u) != 0u);
        SW_I2C_Delay();
        SW_I2C_SCL(1u); SW_I2C_Delay();
        SW_I2C_SCL(0u); SW_I2C_Delay();
        Byte <<= 1;
    }
    SW_I2C_SDA(1u); SW_I2C_Delay();
    SW_I2C_SCL(1u); SW_I2C_Delay();
    SW_I2C_SCL(0u); SW_I2C_Delay();
}

static void OLED_WriteCmd(uint8_t Cmd)
{
    SW_I2C_Start();
    SW_I2C_SendByte((uint8_t)(OLED_I2C_ADDR_7BIT << 1));   /* 写方向 */
    SW_I2C_SendByte(OLED_CMD_BYTE);                        /* 命令流 */
    SW_I2C_SendByte(Cmd);
    SW_I2C_Stop();
}

static void OLED_WriteDataBuf(const uint8_t *Buf, uint16_t Len)
{
    SW_I2C_Start();
    SW_I2C_SendByte((uint8_t)(OLED_I2C_ADDR_7BIT << 1));
    SW_I2C_SendByte(OLED_DATA_BYTE);                       /* 数据流 */
    while (Len--)
    {
        SW_I2C_SendByte(*Buf++);
    }
    SW_I2C_Stop();
}

/*------------------------------------------------------------------------------
 * GPIO 初始化：SCL/SDA 开漏输出 + 内部上拉（模块上通常已有 4.7k 上拉）
 *----------------------------------------------------------------------------*/
void OLED_GPIO_Init(void)
{
    GPIO_InitTypeDef gpio;

    __HAL_RCC_GPIOB_CLK_ENABLE();

    gpio.Mode  = GPIO_MODE_OUTPUT_OD;
    gpio.Pull  = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Pin   = OLED_SCL_PIN;
    HAL_GPIO_Init(OLED_SCL_PORT, &gpio);
    gpio.Pin   = OLED_SDA_PIN;
    HAL_GPIO_Init(OLED_SDA_PORT, &gpio);

    HAL_GPIO_WritePin(OLED_SCL_PORT, OLED_SCL_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(OLED_SDA_PORT, OLED_SDA_PIN, GPIO_PIN_SET);
}

/*------------------------------------------------------------------------------
 * 初始化
 *----------------------------------------------------------------------------*/
void OLED_Init(void)
{
    uint8_t i;

    OLED_GPIO_Init();
    HAL_Delay(50u);                       /* 等模块上电稳定 */

    for (i = 0u; i < (uint8_t)(sizeof(s_InitCmd)); i++)
    {
        OLED_WriteCmd(s_InitCmd[i]);
    }

    /* 打开全屏写入窗口：列 0~127，页 0~7 */
    OLED_WriteCmd(0x21); OLED_WriteCmd(0x00); OLED_WriteCmd(0x7F);
    OLED_WriteCmd(0x22); OLED_WriteCmd(0x00); OLED_WriteCmd(0x07);

    OLED_Clear();
    OLED_GRAM_Refresh();
}

/*------------------------------------------------------------------------------
 * 填充 / 清屏
 *----------------------------------------------------------------------------*/
void OLED_Fill(uint8_t Color)
{
    uint16_t i;
    for (i = 0u; i < (OLED_PAGE_NUM * OLED_WIDTH); i++)
    {
        s_GRAM[i] = Color ? 0xFFu : 0x00u;
    }
}

void OLED_Clear(void)
{
    OLED_Fill(OLED_COLOR_BLACK);
}

/*------------------------------------------------------------------------------
 * 定位：Page(0~7) Col(0~127)
 *----------------------------------------------------------------------------*/
void OLED_SetCursor(uint8_t Page, uint8_t Col)
{
    OLED_WriteCmd((uint8_t)(0xB0u | (Page & 0x07u)));
    OLED_WriteCmd((uint8_t)(0x00u | (Col & 0x0Fu)));
    OLED_WriteCmd((uint8_t)(0x10u | ((Col >> 4) & 0x0Fu)));
}

/*------------------------------------------------------------------------------
 * 画点（坐标可为整屏像素原点 0,0）
 *----------------------------------------------------------------------------*/
void OLED_DrawPixel(int16_t X, int16_t Y, uint8_t Color)
{
    if ((X < 0) || (X >= OLED_WIDTH) || (Y < 0) || (Y >= OLED_HEIGHT)) { return; }

    if (Color)
    {
        s_GRAM[(uint16_t)(Y / 8u) * OLED_WIDTH + (uint16_t)X] |= (uint8_t)(1u << (Y % 8u));
    }
    else
    {
        s_GRAM[(uint16_t)(Y / 8u) * OLED_WIDTH + (uint16_t)X] &= (uint8_t)~(1u << (Y % 8u));
    }
}

void OLED_DrawHLine(int16_t X0, int16_t X1, int16_t Y, uint8_t Color)
{
    int16_t x;
    if (X0 > X1) { int16_t t = X0; X0 = X1; X1 = t; }
    for (x = X0; x <= X1; x++) { OLED_DrawPixel(x, Y, Color); }
}

void OLED_DrawVLine(int16_t X, int16_t Y0, int16_t Y1, uint8_t Color)
{
    int16_t y;
    if (Y0 > Y1) { int16_t t = Y0; Y0 = Y1; Y1 = t; }
    for (y = Y0; y <= Y1; y++) { OLED_DrawPixel(X, y, Color); }
}

void OLED_DrawRect(int16_t X, int16_t Y, int16_t W, int16_t H, uint8_t Color)
{
    OLED_DrawHLine(X, (int16_t)(X + W - 1), Y, Color);
    OLED_DrawHLine(X, (int16_t)(X + W - 1), (int16_t)(Y + H - 1), Color);
    OLED_DrawVLine(X, Y, (int16_t)(Y + H - 1), Color);
    OLED_DrawVLine((int16_t)(X + W - 1), Y, (int16_t)(Y + H - 1), Color);
}

/*------------------------------------------------------------------------------
 * 通用位图绘制（字模格式：行优先，每行 ceil(W/8) 字节，MSB = 最左像素）
 *   - 完全在屏幕外时直接返回，避免为每个像素做一次边界判断（滚动时经常发生）
 *   - 部分在屏幕外时由 OLED_DrawPixel 自动裁剪
 *----------------------------------------------------------------------------*/
void OLED_DrawBitmap(int16_t X, int16_t Y, const uint8_t *Bmp,
                     uint8_t W, uint8_t H, uint8_t Color)
{
    uint8_t perRow = (uint8_t)((W + 7u) / 8u);
    uint8_t row, col;

    if ((Bmp == (const uint8_t *)0) || (W == 0u) || (H == 0u)) { return; }
    if (((int16_t)(X + (int16_t)W) <= 0) || (X >= OLED_WIDTH))  { return; }
    if (((int16_t)(Y + (int16_t)H) <= 0) || (Y >= OLED_HEIGHT)) { return; }

    for (row = 0u; row < H; row++)
    {
        const uint8_t *line = &Bmp[(uint16_t)row * perRow];
        for (col = 0u; col < W; col++)
        {
            if ((line[col >> 3] & (uint8_t)(0x80u >> (col & 7u))) != 0u)
            {
                OLED_DrawPixel((int16_t)(X + (int16_t)col), (int16_t)(Y + (int16_t)row), Color);
            }
        }
    }
}

/*------------------------------------------------------------------------------
 * 5x7 字符 / 字符串
 *----------------------------------------------------------------------------*/
void OLED_PutChar(uint8_t X, uint8_t Y, char Ch, uint8_t Color)
{
    const uint8_t *glyph = Font5x7[FONT5X7_INDEX((uint8_t)Ch)];
    uint8_t row, col;

    for (row = 0u; row < FONT5X7_H; row++)
    {
        uint8_t bits = glyph[row];
        for (col = 0u; col < FONT5X7_W; col++)
        {
            if (bits & (uint8_t)(1u << (FONT5X7_W - 1u - col)))
            {
                OLED_DrawPixel((int16_t)(X + col), (int16_t)(Y + row), Color);
            }
        }
    }
}

void OLED_PrintWidth(const char *Str, uint8_t *Width)
{
    uint8_t len = 0u;
    while (Str[len] != '\0')
    {
        len++;
    }
    *Width = (uint8_t)(len * FONT5X7_ADV);
}

void OLED_Print(uint8_t X, uint8_t Y, const char *Str, uint8_t Color)
{
    while (*Str != '\0')
    {
        OLED_PutChar(X, Y, *Str, Color);
        X = (uint8_t)(X + FONT5X7_ADV);
        Str++;
    }
}

void OLED_PrintCentered(int16_t X, int16_t Y, const char *Str, uint8_t Color)
{
    uint8_t width;
    OLED_PrintWidth(Str, &width);
    OLED_Print((uint8_t)(((int16_t)X >= (int16_t)width) ? ((int16_t)X - (int16_t)width) / 2 : 0),
               (uint8_t)Y, Str, Color);
}

/*------------------------------------------------------------------------------
 * 放大字符（Scale = 2/3/4...；Bold = 沿 X 方向再补 1 像素，实现“粗体”）
 *   - 单个源像素 -> Scale x Scale 的实心方块
 *   - Bold 时向右多画一列，使竖笔画变粗
 *----------------------------------------------------------------------------*/
void OLED_DrawBigChar(uint8_t X, uint8_t Y, char Ch, uint8_t Scale, uint8_t Bold, uint8_t Color)
{
    const uint8_t *glyph = Font5x7[FONT5X7_INDEX((uint8_t)Ch)];
    uint8_t row, col, sx, sy;

    for (row = 0u; row < FONT5X7_H; row++)
    {
        uint8_t bits = glyph[row];
        for (col = 0u; col < FONT5X7_W; col++)
        {
            if (bits & (uint8_t)(1u << (FONT5X7_W - 1u - col)))
            {
                uint8_t px = (uint8_t)(X + col * Scale);
                uint8_t py = (uint8_t)(Y + row * Scale);
                for (sy = 0u; sy < Scale; sy++)
                {
                    for (sx = 0u; sx < Scale; sx++)
                    {
                        OLED_DrawPixel((int16_t)(px + sx), (int16_t)(py + sy), Color);
                    }
                    if (Bold)
                    {
                        OLED_DrawPixel((int16_t)(px + Scale), (int16_t)(py + sy), Color);
                    }
                }
            }
        }
    }
}

/* 放大字符的水平步进（含字符间隔），冒号本身窄，给少一点 */
void OLED_BigStringAdvance(char Ch, uint8_t Scale, uint8_t Bold, uint8_t *Advance)
{
    if (Ch == ':')
    {
        *Advance = (uint8_t)(3u * Scale + (Bold ? 2u : 1u));
    }
    else
    {
        *Advance = (uint8_t)(FONT5X7_W * Scale + (Bold ? 2u : 1u));
    }
}

void OLED_DrawBigString(uint8_t X, uint8_t Y, const char *Str, uint8_t Scale, uint8_t Bold, uint8_t Color)
{
    uint8_t x = X;
    while (*Str != '\0')
    {
        uint8_t adv;
        OLED_DrawBigChar(x, (uint8_t)Y, *Str, Scale, Bold, Color);
        OLED_BigStringAdvance(*Str, Scale, Bold, &adv);
        x = (uint8_t)(x + adv);
        Str++;
    }
}

void OLED_DrawBigStringCentered(int16_t Y, const char *Str, uint8_t Scale, uint8_t Bold, uint8_t Color)
{
    uint16_t total = 0u;
    const char *p = Str;

    while (*p != '\0')                       /* 先算总宽，再居中 */
    {
        uint8_t adv;
        OLED_BigStringAdvance(*p, Scale, Bold, &adv);
        total = (uint16_t)(total + adv);
        p++;
    }
    {
        int32_t x = ((int32_t)OLED_WIDTH - (int32_t)total) / 2;
        if (x < 0) { x = 0; }
        OLED_DrawBigString((uint8_t)x, (uint8_t)Y, Str, Scale, Bold, Color);
    }
}

/*------------------------------------------------------------------------------
 * 刷新：整屏一次性推送（8 页 * 128 字节）
 *----------------------------------------------------------------------------*/
void OLED_GRAM_Refresh(void)
{
    uint8_t page;

    for (page = 0u; page < OLED_PAGE_NUM; page++)
    {
        OLED_WriteCmd((uint8_t)(0xB0u | page));       /* 设置页起始 */
        OLED_WriteDataBuf(&s_GRAM[page * OLED_WIDTH], OLED_WIDTH);
    }
}
