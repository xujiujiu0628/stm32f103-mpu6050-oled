/**
  ******************************************************************************
  * @file    MyI2C.c
  * @brief   软件模拟 I2C 主机（驱动 SSD1306 OLED，PB8=SCL / PB9=SDA）
  *
  * @移植说明 本文件源自 SPL 工程《16-2OLED-4针脚I2C接口-使用缓存区》，已移植到 HAL 库：
  *          仅替换硬件接触层——时钟使能/GPIO 配置与读写改用 HAL API，
  *          Delay_us 改为基于 DWT 周期计数器（CYCCNT）的微秒级忙等；
  *          I2C 时序逻辑（Start/Stop/SendByte 等）与原工程逐行一致，未做改动。
  *          注意：软件 I2C 为开漏输出，依赖 OLED 模块板载上拉电阻（通常 4.7kΩ）。
  ******************************************************************************
  */
#include "stm32f1xx_hal.h"              // STM32F1 HAL 库头文件

/* ---------------- 可移植配置：更换引脚/调速只需修改以下宏 ---------------- */
#define MYI2C_SCL_PORT   GPIOB          // SCL 引脚端口（注意：SCL/SDA 须在同一端口）
#define MYI2C_SCL_PIN    GPIO_PIN_8     // SCL = PB8
#define MYI2C_SDA_PORT   GPIOB          // SDA 引脚端口
#define MYI2C_SDA_PIN    GPIO_PIN_9     // SDA = PB9
#define MYI2C_DELAY_US   3U             // 每位延时(μs)。注意：一个 SCL 周期调 3 次延时函数
                                        //（W_SDA + W_SCL(1) + W_SCL(0)），叠加 HAL_GPIO_WritePin 开销，
                                        // 实测等效约 63kHz、128us/字节、整帧 132ms（2026-10-26 实测）。
                                        // 本文件上方"3≈330kHz / 整帧 13ms"的旧说法是错的（没实测过），
                                        // 已按真机数据订正；不要据此估主循环预算。
                                        // 整帧阻塞已由 oled_view.c 的分片刷新（64 字节/片）摊平。
                                        // 若通信不稳（花屏/坏字）回调 5，再不行 10

/**
  * @brief  微秒级忙等延时（HAL_Delay 最小分辨率 1ms，无法用于 I2C 位时序）
  * @note   基于 DWT 周期计数器：72MHz 主频下每 μs 计 72 个周期；
  *         无符号数减法天然抗 32 位回绕。调用前须在 MyI2C_Init 中使能 CYCCNT。
  */
static void i2c_delay_us(uint32_t us)
{
	uint32_t start = DWT->CYCCNT;                        // 记录起始周期计数
	uint32_t ticks = us * (SystemCoreClock / 1000000U);  // 目标等待的周期数（SystemCoreClock=72MHz）
	while ((DWT->CYCCNT - start) < ticks)                // 忙等到期
	{
	}
}

// 写SCL引脚电平函数
void MyI2C_W_SCL(uint8_t BitValue)
{
	HAL_GPIO_WritePin(MYI2C_SCL_PORT, MYI2C_SCL_PIN, (GPIO_PinState)BitValue);  // 设置SCL引脚电平
	i2c_delay_us(MYI2C_DELAY_US);  // 延时保证I2C时序稳定
}

// 写SDA引脚电平函数
void MyI2C_W_SDA(uint8_t BitValue)
{
	HAL_GPIO_WritePin(MYI2C_SDA_PORT, MYI2C_SDA_PIN, (GPIO_PinState)BitValue);  // 设置SDA引脚电平
	i2c_delay_us(MYI2C_DELAY_US);  // 延时保证I2C时序稳定
}

// 读SDA引脚电平函数
uint8_t MyI2C_R_SDA(void)
{
	uint8_t BitValue;
	// 开漏模式下读引脚实际电平：高电平=总线被上拉释放，低电平=从机拉低应答
	BitValue = (HAL_GPIO_ReadPin(MYI2C_SDA_PORT, MYI2C_SDA_PIN) == GPIO_PIN_SET) ? 1 : 0;
	i2c_delay_us(MYI2C_DELAY_US);  // 延时保证I2C时序稳定
	return BitValue;  // 返回引脚电平状态
}

// I2C初始化函数
void MyI2C_Init(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;

	// 使能 DWT 周期计数器（Cortex-M3 内核调试单元），供 i2c_delay_us 使用；
	// 若不使能，CYCCNT 恒为 0 会导致延时函数死等
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;  // 打开调试跟踪系统使能位
	DWT->CYCCNT = 0;                                 // 周期计数器清零
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;             // 启动 CYCCNT 开始计数

	// 使能GPIOB端口时钟
	__HAL_RCC_GPIOB_CLK_ENABLE();

	// 配置引脚为开漏输出模式
	GPIO_InitStructure.Pin   = MYI2C_SCL_PIN | MYI2C_SDA_PIN;  // SCL(PB8)和SDA(PB9)引脚
	GPIO_InitStructure.Mode  = GPIO_MODE_OUTPUT_OD;  // 开漏输出模式（支持双向通信）
	GPIO_InitStructure.Pull  = GPIO_NOPULL;          // 不用内部上拉，靠模块板载上拉
	GPIO_InitStructure.Speed = GPIO_SPEED_FREQ_HIGH; // 高速档（对应原工程 50MHz）
	HAL_GPIO_Init(MYI2C_SCL_PORT, &GPIO_InitStructure);          // 应用GPIO配置

	// 初始化时将SCL和SDA引脚置高（I2C总线空闲状态）
	HAL_GPIO_WritePin(MYI2C_SCL_PORT, MYI2C_SCL_PIN, GPIO_PIN_SET);
	HAL_GPIO_WritePin(MYI2C_SDA_PORT, MYI2C_SDA_PIN, GPIO_PIN_SET);
}

// I2C起始信号函数
void MyI2C_Start(void)
{
	MyI2C_W_SDA(1);  // 确保SDA为高电平
	MyI2C_W_SCL(1);  // 确保SCL为高电平
	MyI2C_W_SDA(0);  // SDA产生下降沿（起始条件）
	MyI2C_W_SCL(0);  // 拉低SCL，准备数据传输
}

// I2C停止信号函数
void MyI2C_Stop(void)
{
	MyI2C_W_SDA(0);  // 确保SDA为低电平
	MyI2C_W_SCL(1);  // SCL为高电平
	MyI2C_W_SDA(1);  // SDA产生上升沿（停止条件）
}

// I2C发送一个字节函数
void MyI2C_SendByte(uint8_t Byte)
{
	uint8_t i;
	for(i = 0; i < 8; i++)  // 循环8次，发送8位数据
	{
		// 从高位到低位依次发送
		MyI2C_W_SDA(Byte & (0x80 >> i));  // 提取当前位并设置SDA
		MyI2C_W_SCL(1);                   // 拉高SCL，数据被采样
		MyI2C_W_SCL(0);                   // 拉低SCL，准备下一位
	}
}

// I2C接收一个字节函数
uint8_t MyI2C_ReceiveByte(void)
{
	uint8_t i, Byte = 0x00;
	MyI2C_W_SDA(1);  // 释放SDA线，设置为输入模式（开漏输出的高电平状态）
	for(i = 0; i < 8; i++)  // 循环8次，接收8位数据
	{
		MyI2C_W_SCL(1);  // 拉高SCL，从设备放置数据位
		if(MyI2C_R_SDA() == 1)  // 读取SDA电平
		{
			Byte |= (0x80 >> i);  // 如果为高电平，设置对应位
		}
		MyI2C_W_SCL(0);  // 拉低SCL，准备下一位
	}
	return Byte;  // 返回接收到的字节
}

// I2C发送应答位函数
void MyI2C_SendAck(uint8_t AckBit)
{
	MyI2C_W_SDA(AckBit);  // 设置应答位（0-应答，1-非应答）
	MyI2C_W_SCL(1);       // 拉高SCL，应答位被采样
	MyI2C_W_SCL(0);       // 拉低SCL
}

// I2C接收应答位函数
uint8_t MyI2C_ReceiveAck(void)
{
	uint8_t AckBit;
	MyI2C_W_SDA(1);       // 释放SDA线，准备读取应答
	MyI2C_W_SCL(1);       // 拉高SCL
	AckBit = MyI2C_R_SDA();  // 读取应答位
	MyI2C_W_SCL(0);       // 拉低SCL
	return AckBit;        // 返回应答位（0-应答，1-非应答）
}
