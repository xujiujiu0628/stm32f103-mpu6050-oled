/**
  ******************************************************************************
  * @file    mpu6050_i2c.c
  * @brief   MPU6050 专用软件 I2C（PB10=SCL / PB11=SDA）
  *
  * @note    1. 复用 adc-oled MyI2C 的实现模式（DWT 延时 + HAL GPIO + 开漏输出）
  *             但不复用其代码——MPU6050 通信地址探测 + 写寄存器 + 读 burst 序列
  *             与 OLED 命令流差异大，独立实现便于单测和维护。
  *          2. I2C 速率 400kHz：MPU6050 采样 100Hz + OLED 10Hz 刷新总 110 次/秒，
  *             双总线独立，软件 I2C 短帧 < 1ms，CPU 占用 < 15%。
  *          3. 总线独立：OLED 在 PB8/PB9，MPU6050 在 PB10/PB11，互不干扰，
  *             同一时刻只有一条总线在驱动。
  ******************************************************************************
  */
#include "mpu6050_i2c.h"

/* ---------------- 可移植配置：MPU6050 专用 I2C 引脚 ---------------- */
#define MPU6050_SCL_PORT   GPIOB          // SCL 引脚端口
#define MPU6050_SCL_PIN    GPIO_PIN_10    // SCL = PB10
#define MPU6050_SDA_PORT   GPIOB          // SDA 引脚端口
#define MPU6050_SDA_PIN    GPIO_PIN_11    // SDA = PB11
#define MPU6050_I2C_DELAY_US   2U         // 400kHz 速率档（每位 2.5μs，含 set+hold 留余量）

/**
  * @brief  微秒级忙等延时（与 MyI2C.c 实现等价，但 DWT 必须在 main 中先使能）
  */
static void i2c_delay_us(uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t ticks = us * (SystemCoreClock / 1000000U);
    while ((DWT->CYCCNT - start) < ticks)
    {
    }
}

static void i2c_w_scl(uint8_t bit)
{
    HAL_GPIO_WritePin(MPU6050_SCL_PORT, MPU6050_SCL_PIN, (GPIO_PinState)bit);
    i2c_delay_us(MPU6050_I2C_DELAY_US);
}

static void i2c_w_sda(uint8_t bit)
{
    HAL_GPIO_WritePin(MPU6050_SDA_PORT, MPU6050_SDA_PIN, (GPIO_PinState)bit);
    i2c_delay_us(MPU6050_I2C_DELAY_US);
}

static uint8_t i2c_r_sda(void)
{
    uint8_t v = (HAL_GPIO_ReadPin(MPU6050_SDA_PORT, MPU6050_SDA_PIN) == GPIO_PIN_SET) ? 1 : 0;
    i2c_delay_us(MPU6050_I2C_DELAY_US);
    return v;
}

void MPU6050_I2C_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();

    gpio.Pin   = MPU6050_SCL_PIN | MPU6050_SDA_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_OD;
    gpio.Pull  = GPIO_NOPULL;            // 依赖 GY-521 模块板载 4.7kΩ 上拉
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(MPU6050_SCL_PORT, &gpio);

    HAL_GPIO_WritePin(MPU6050_SCL_PORT, MPU6050_SCL_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(MPU6050_SDA_PORT, MPU6050_SDA_PIN, GPIO_PIN_SET);
}

void MPU6050_I2C_Start(void)
{
    i2c_w_sda(1);
    i2c_w_scl(1);
    i2c_w_sda(0);   // 下降沿 = 起始
    i2c_w_scl(0);
}

void MPU6050_I2C_Stop(void)
{
    i2c_w_sda(0);
    i2c_w_scl(1);
    i2c_w_sda(1);   // 上升沿 = 停止
}

void MPU6050_I2C_SendByte(uint8_t byte)
{
    for (uint8_t i = 0; i < 8; i++)
    {
        i2c_w_sda(byte & (0x80 >> i));
        i2c_w_scl(1);
        i2c_w_scl(0);
    }
}

uint8_t MPU6050_I2C_ReceiveByte(void)
{
    uint8_t byte = 0x00;
    i2c_w_sda(1);   // 释放总线
    for (uint8_t i = 0; i < 8; i++)
    {
        i2c_w_scl(1);
        if (i2c_r_sda() == 1)
        {
            byte |= (0x80 >> i);
        }
        i2c_w_scl(0);
    }
    return byte;
}

void MPU6050_I2C_SendAck(uint8_t ack)
{
    i2c_w_sda(ack);
    i2c_w_scl(1);
    i2c_w_scl(0);
}

uint8_t MPU6050_I2C_ReceiveAck(void)
{
    uint8_t ack;
    i2c_w_sda(1);
    i2c_w_scl(1);
    ack = i2c_r_sda();
    i2c_w_scl(0);
    return ack;
}
