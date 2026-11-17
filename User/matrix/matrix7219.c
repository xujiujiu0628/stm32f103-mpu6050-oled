/**
  ******************************************************************************
  * @file    matrix7219.c
  * @brief   MAX7219 8x8 点阵驱动（软件位翻转：PA6=CLK、PA7=DIN、PA4=CS）
  *
  * @时序    MAX7219 在 CLK 上升沿采样 DIN（datasheet）：
  *          数据先摆好 → CLK 拉高（采样）→ CLK 拉低（换下一位）。
  *          2026-10-26 真机踩坑留档：模式写错（2EDGE 等价）会让比特流错位一位
  *          → shutdown 寄存器写不进 → 芯片停在上电默认关显 → 点阵全黑。
  *          16bit 帧 = [D15..D12 无效][D11..D8 寄存器][D7..D0 数据]，
  *          MSB first，CS 上升沿锁存。
  * @速率    软件翻转，每边沿插 2 条 NOP（72MHz 下约 60ns），远低于 datasheet 10MHz 上限
  ******************************************************************************
  */
#include "matrix7219.h"
#include "stm32f1xx_hal.h"

/* MAX7219 寄存器地址 */
#define MAX7219_REG_DECODE    0x09
#define MAX7219_REG_INTENSITY 0x0A
#define MAX7219_REG_SCANLIMIT 0x0B
#define MAX7219_REG_SHUTDOWN  0x0C
#define MAX7219_REG_DISPTEST  0x0F
#define MAX7219_REG_DIGIT0    0x01   /* DIG0..DIG7 = 0x01..0x08 */

/* 2026-11-17 引脚重排（用户要求）：电位器挪到 PA5/ADC1_IN5，PA6 改为点阵 CLK。
 * 硬件 SPI1 的 SCK 固定在 PA5，腾不出来 → 点阵改为软件位翻转：
 *   PA6 = CLK、PA7 = DIN、PA4 = CS（软件片控，低有效）。
 * 帧时序同原 SPI 模式 0：CS 低 → 16bit MSB first → CS 上升沿锁存。 */
#define MX_CLK_PIN   GPIO_PIN_6
#define MX_DIN_PIN   GPIO_PIN_7
#define MX_CS_PIN    GPIO_PIN_4

/* MAX7219 时序下限（V+=5V datasheet）: tCH/tCL ≥ 50ns, tDS/tDH ≥ 25ns。
 * 72MHz 下 1 NOP ≈ 13.9ns → 8 条 ≈ 110ns，留一倍余量。
 * 2026-11-17 真机: 首版只插 2 条 NOP(≈28ns) 低于 tCH 下限，点阵全黑。 */
static void mx_delay(void)
{
    __NOP(); __NOP(); __NOP(); __NOP();
    __NOP(); __NOP(); __NOP(); __NOP();
}

static void mx_send16(uint16_t v)
{
    int i;
    for (i = 15; i >= 0; i--) {
        HAL_GPIO_WritePin(GPIOA, MX_DIN_PIN,
                          (v & (1U << i)) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        mx_delay();                                           /* DIN 建立 ≥25ns */
        HAL_GPIO_WritePin(GPIOA, MX_CLK_PIN, GPIO_PIN_SET);   /* 上升沿采样 */
        mx_delay();                                           /* 高电平 ≥50ns */
        HAL_GPIO_WritePin(GPIOA, MX_CLK_PIN, GPIO_PIN_RESET);
        mx_delay();                                           /* 低电平 ≥50ns */
    }
}

uint8_t Matrix7219_WriteRegister(uint8_t reg, uint8_t val)
{
    HAL_GPIO_WritePin(GPIOA, MX_CS_PIN, GPIO_PIN_RESET);
    mx_send16((uint16_t)(((uint16_t)reg << 8) | val));
    HAL_GPIO_WritePin(GPIOA, MX_CS_PIN, GPIO_PIN_SET);   /* 上升沿锁存 */
    return 0;                    /* 软件翻转没有 HAL 状态码可报，恒 0 */
}

/* 当前亮度档（0x0~0xF）。由电位器三档驱动，见 adc_brt.c；
 * 重发配置时用它而不是常量，否则上电那 5 秒的重发会把亮度打回默认。 */
static uint8_t s_intensity = MATRIX_INTENSITY_MID;

/* 配置序列（decode/scan/intensity/shutdown）。抽出来是为了能在电源稳定后
 * 再下发一遍：2026-11-17 真机实测，上电瞬间那串写在模块 5V 还没稳时被芯片
 * 丢掉 → 停在 shutdown 状态 —— 表现为"上电亮一下（display test 照常生效）、
 * 之后全黑（数字寄存器在 shutdown 下不显示）"。 */
static void mx_config(void)
{
    Matrix7219_WriteRegister(MAX7219_REG_SHUTDOWN,  0x00);  /* 配置期间先关显示 */
    Matrix7219_WriteRegister(MAX7219_REG_DECODE,    0x00);  /* BCD 译码关：直接点阵模式 */
    Matrix7219_WriteRegister(MAX7219_REG_SCANLIMIT, 0x07);  /* 8 行全扫 */
    Matrix7219_WriteRegister(MAX7219_REG_INTENSITY, s_intensity);
    Matrix7219_WriteRegister(MAX7219_REG_SHUTDOWN,  0x01);  /* 正常工作 */
}

uint8_t Matrix7219_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* PA6=CLK / PA7=DIN / PA4=CS：全部推挽输出；CS 初始拉高（未选中），CLK 空闲低 */
    gpio.Pin   = MX_CLK_PIN | MX_DIN_PIN | MX_CS_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);
    HAL_GPIO_WritePin(GPIOA, MX_CS_PIN,  GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOA, MX_CLK_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOA, MX_DIN_PIN, GPIO_PIN_RESET);

    mx_config();

    {
        uint8_t blank[8] = {0};
        Matrix7219_ShowFrame(blank);
    }
    return 0;
}

/* 电源稳定后重新下发配置（见 mx_config 注释）。幂等，可重复调用。 */
void Matrix7219_Reconfig(void)
{
    mx_config();
    {
        uint8_t blank[8] = {0};
        Matrix7219_ShowFrame(blank);
    }
}

void Matrix7219_DisplayTest(uint8_t on)
{
    Matrix7219_WriteRegister(MAX7219_REG_DISPTEST, on ? 0x01 : 0x00);
}

void Matrix7219_ShowFrame(const uint8_t frame[8])
{
    uint8_t i;
    for (i = 0; i < 8; i++)
        Matrix7219_WriteRegister(MAX7219_REG_DIGIT0 + i, frame[i]);
}

void Matrix7219_SetIntensity(uint8_t intensity)
{
    if (intensity > 0x0F) intensity = 0x0F;
    s_intensity = intensity;                     /* 记住档位，供重发配置使用 */
    Matrix7219_WriteRegister(MAX7219_REG_INTENSITY, intensity);
}

uint8_t Matrix7219_GetIntensity(void)
{
    return s_intensity;
}

void Matrix7219_Shutdown(uint8_t off)
{
    Matrix7219_WriteRegister(MAX7219_REG_SHUTDOWN, off ? 0x00 : 0x01);
}
