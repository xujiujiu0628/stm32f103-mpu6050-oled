/**
  ******************************************************************************
  * @file    matrix7219.c
  * @brief   MAX7219 8x8 点阵驱动（SPI1 硬件外设，PA4=CS 软件片控）
  *
  * @时序    MAX7219 采样 CLK 上升沿，CPOL=1/CPHA=0 或 CPOL=0/CPHA=1 均可
  *          （只要数据在上升沿前建立）；16bit 帧 = [D15..D12 无效][D11..D8 寄存器][D7..D0 数据]，
  *          MSB first，CS 上升沿锁存。SPI 预分频 8 → 9MHz < 10MHz 上限。
  ******************************************************************************
  */
#include "matrix7219.h"
#include "stm32f1xx_hal.h"

/* MAX7219 寄存器地址 */
#define MAX7219_REG_DECODE    0x09
#define MAX7219_REG_INTENSITY 0x0A
#define MAX7219_REG_SCANLIMIT 0x0B
#define MAX7219_REG_SHUTDOWN  0x0C
#define MAX7219_REG_DIGIT0    0x01   /* DIG0..DIG7 = 0x01..0x08 */

static SPI_HandleTypeDef s_hspi;

void Matrix7219_WriteRegister(uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { reg, val };
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    HAL_SPI_Transmit(&s_hspi, tx, 2, 10);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);
}

void Matrix7219_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_SPI1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* PA5=SCK, PA7=MOSI：复用推挽；PA4=CS：软件控制推挽，初始拉高（未选中） */
    gpio.Pin   = GPIO_PIN_5 | GPIO_PIN_7;
    gpio.Mode  = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);

    gpio.Pin  = GPIO_PIN_4;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    HAL_GPIO_Init(GPIOA, &gpio);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);

    s_hspi.Instance               = SPI1;
    s_hspi.Init.Mode              = SPI_MODE_MASTER;
    s_hspi.Init.Direction         = SPI_DIRECTION_2LINES;   /* F1 HAL 无 TXONLY，MISO 不接即可 */
    s_hspi.Init.DataSize          = SPI_DATASIZE_8BIT;
    s_hspi.Init.CLKPolarity       = SPI_POLARITY_LOW;
    s_hspi.Init.CLKPhase          = SPI_PHASE_2EDGE;   /* CPOL=0/CPHA=1：上升沿采样 */
    s_hspi.Init.NSS               = SPI_NSS_SOFT;
    s_hspi.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;   /* 72/8 = 9MHz */
    s_hspi.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    HAL_SPI_Init(&s_hspi);
    __HAL_SPI_ENABLE(&s_hspi);

    /* MAX7219 上电默认 shutdown + 全灭，按 datasheet 顺序初始化 */
    Matrix7219_WriteRegister(MAX7219_REG_SHUTDOWN,  0x00);   /* 配置期间先关显示 */
    Matrix7219_WriteRegister(MAX7219_REG_DECODE,    0x00);   /* BCD 译码关：直接点阵模式 */
    Matrix7219_WriteRegister(MAX7219_REG_SCANLIMIT, 0x07);   /* 8 行全扫 */
    Matrix7219_WriteRegister(MAX7219_REG_INTENSITY, MATRIX_INTENSITY_MID);
    Matrix7219_WriteRegister(MAX7219_REG_SHUTDOWN,  0x01);   /* 正常工作 */

    {
        uint8_t blank[8] = {0};
        Matrix7219_ShowFrame(blank);
    }
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
    Matrix7219_WriteRegister(MAX7219_REG_INTENSITY, intensity);
}

void Matrix7219_Shutdown(uint8_t off)
{
    Matrix7219_WriteRegister(MAX7219_REG_SHUTDOWN, off ? 0x00 : 0x01);
}
