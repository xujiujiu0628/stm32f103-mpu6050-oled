/**
  ******************************************************************************
  * @file    adc_brt.c
  * @brief   电位器亮度控制实现（采样层；档位逻辑见 brt_band.c 纯函数）
  *
  * @硬件    PA6 = ADC1_IN6。电位器三脚：中间抽头→PA6，两端→3V3/GND。
  *
  * @要点（沿用 adc-oled bsp_adc 趟平的 F1 HAL 路线，防再踩）：
  *   - ADCCLK = PCLK2/6 = 12MHz（<14MHz 上限）
  *   - 引脚 GPIO_MODE_ANALOG 断开施密特触发
  *   - 上电校准一次（HAL_ADCEx_Calibration_Start）
  *   - 判 EOC 用 HAL_ADC_PollForConversion（本 HAL 无 __HAL_ADC_GET_FLAG）
  *   - 启用 ADC 必须连带编译 hal_dma.c（F1 句柄内嵌 DMA_HandleTypeDef，
  *     Makefile 既有 hal_dma.c 在列，勿删）
  *
  * @策略    每次 Poll 采一帧（~68μs），8 点滑动平均；每 50ms 用均值走一次
  *          AdcBrt_NextBand——判决延迟 ≤50ms，拧动跟手；档位变化才发
  *          0x81+值 两条软 I2C 命令（~0.3ms），杜绝周期性刷屏占用。
  *          档位变化同时打 RTT `[BRT] band=X mv=NNNN` 行作机器断言锚。
  *          关屏（ScreenOn=0）时只记档不发命令，唤醒补发。
  ******************************************************************************
  */
#include "adc_brt.h"
#include "My_OLED.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>
#include "SEGGER_RTT.h"

extern volatile uint32_t g_millis;

#define BRT_ADC_DEV        ADC1
#define BRT_ADC_CH         ADC_CHANNEL_6        /* PA6 */
#define BRT_EVAL_MS        50U
#define BRT_AVG_N          8U

static ADC_HandleTypeDef s_hadc;
static uint8_t    s_adc_ok     = 0;            /* init 失败则模块静默降级 */
static Brt_Band_t s_band       = BRT_BAND_MID;  /* 上电默认 = 屏出厂对比度 */
static uint8_t    s_screen_on  = 1;
static uint8_t    s_cmd_pending = 1;           /* 对比度命令待发（变档/上电） */
static uint16_t   s_ring[BRT_AVG_N];
static uint8_t    s_ring_i     = 0;
static uint8_t    s_ring_fill  = 0;
static uint16_t   s_avg_mv     = 0;
static uint32_t   s_next_eval  = 0;
static uint32_t   s_next_log   = 0;            /* [BRT] 心跳节拍 1s */

static uint16_t adc_read_mv(void)
{
    ADC_ChannelConfTypeDef ch = {0};
    uint32_t raw;

    ch.Channel      = BRT_ADC_CH;
    ch.Rank         = ADC_REGULAR_RANK_1;
    ch.SamplingTime = ADC_SAMPLETIME_55CYCLES_5;   /* 55.5+12.5 周期 ≈68μs */

    HAL_ADC_ConfigChannel(&s_hadc, &ch);
    HAL_ADC_Start(&s_hadc);
    if (HAL_ADC_PollForConversion(&s_hadc, 10) != HAL_OK)
    {
        HAL_ADC_Stop(&s_hadc);
        return s_avg_mv;          /* 超时: 沿用上帧均值, 不污染滑窗 */
    }
    raw = HAL_ADC_GetValue(&s_hadc);
    HAL_ADC_Stop(&s_hadc);
    /* 12 位 0..4095 ↔ 0..3300mV (定点乘除, 避免浮点) */
    return (uint16_t)((raw * 3300UL) / 4095UL);
}

void AdcBrt_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    RCC_PeriphCLKInitTypeDef pclk = {0};

    __HAL_RCC_ADC1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    gpio.Pin  = GPIO_PIN_6;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);

    pclk.PeriphClockSelection = RCC_PERIPHCLK_ADC;
    pclk.AdcClockSelection    = RCC_ADCPCLK2_DIV6;      /* 72/6=12MHz */
    HAL_RCCEx_PeriphCLKConfig(&pclk);                   /* F1 名: Config 非 Init */

    s_hadc.Instance                   = BRT_ADC_DEV;
    s_hadc.Init.ScanConvMode          = ADC_SCAN_DISABLE;
    s_hadc.Init.ContinuousConvMode    = DISABLE;
    s_hadc.Init.DiscontinuousConvMode = DISABLE;
    s_hadc.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
    s_hadc.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    s_hadc.Init.NbrOfConversion       = 1;
    if (HAL_ADC_Init(&s_hadc) != HAL_OK)
    {
        s_adc_ok = 0;
        SEGGER_RTT_WriteString(0, "[BRT] adc init FAIL (brightness off-line)\r\n");
        return;
    }
    HAL_ADCEx_Calibration_Start(&s_hadc);
    s_adc_ok = 1;
}

static void band_log_apply(Brt_Band_t next, uint32_t now)
{
    static const char *names[BRT_BAND_COUNT] = { "LOW", "MID", "HIGH" };
    char buf[48];

    if (next != s_band)
    {
        s_band        = next;
        s_cmd_pending = 1;                     /* 变档 → 命令待发 */
    }
    if (s_cmd_pending && s_screen_on)
    {
        My_OLED_WriteCommand(0x81);            /* Set Contrast */
        My_OLED_WriteCommand(AdcBrt_ContrastOf(s_band));
        s_cmd_pending = 0;
    }
    /* [BRT] 心跳 1s（FSD FR-BRT-01）: verify 断言锚不得依赖上电首拍的
       RTT 接窗运气——512B 环形缓冲下 banner+STATE 先头连, 一次性行有被
       覆盖风险。1Hz 心跳稳锚且 mv 值全程可观测。对比度命令与心跳解耦,
       仍只在变档下发（FR-BRT-03 刷屏占用禁令不破）。 */
    if ((int32_t)(now - s_next_log) >= 0)
    {
        s_next_log = now + 1000U;
        snprintf(buf, sizeof(buf), "[BRT] band=%s mv=%lu\r\n",
                 names[s_band], (unsigned long)s_avg_mv);
        SEGGER_RTT_WriteString(0, buf);
    }
}

void AdcBrt_Poll(void)
{
    uint32_t now = g_millis;
    uint16_t mv;
    uint32_t sum = 0;
    uint8_t  i;

    if (!s_adc_ok)
    {
        return;                       /* init 失败: 静默降级(已发告警行), 档位保持 MID */
    }
    if ((int32_t)(now - s_next_eval) < 0)
    {
        return;                       /* 未到评估拍 */
    }
    s_next_eval = now + BRT_EVAL_MS;

    /* 采一小段填滑窗: 8 次连续读 ≈0.55ms, 等效对该毫秒窗内均值 */
    for (i = 0; i < BRT_AVG_N; i++)
    {
        mv = adc_read_mv();
        s_ring[s_ring_i] = mv;
        s_ring_i = (uint8_t)((s_ring_i + 1U) % BRT_AVG_N);
        if (s_ring_fill < BRT_AVG_N)
        {
            s_ring_fill++;
        }
    }
    for (i = 0; i < s_ring_fill; i++)
    {
        sum += s_ring[i];
    }
    s_avg_mv = (uint16_t)(sum / s_ring_fill);

    band_log_apply(AdcBrt_NextBand(s_avg_mv, s_band), now);
}

Brt_Band_t AdcBrt_Band(void)       { return s_band; }
uint16_t AdcBrt_Millivolts(void)   { return s_avg_mv; }

void AdcBrt_ScreenOn(uint8_t on)
{
    s_screen_on = on ? 1 : 0;
    if (s_screen_on)
    {
        s_cmd_pending = 1;            /* 唤醒后下一评估拍(≤50ms)补发对比度 */
    }
}
