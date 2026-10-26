/**
  ******************************************************************************
  * @file    main.c
  * @brief   stm32f103-mpu6050-oled：MPU6050 100Hz 采样 + OLED 10Hz 显示 + PA0 按键切换
  *
  * @主循环    单一时间触发：1ms SysTick 维护时间戳 + 子模块 tick
  *           - key_tick: 1ms
  *           - mpu6050_update: 10ms 周期（100Hz 采样 + 互补滤波）
  *           - oled_view_tick: 100ms 周期（10Hz 显示）
  *           - oled_view_handle_key: 按键事件触发（无固定周期）
  *           - rtt_log: 100ms 周期（节流，与显示同频）
  ******************************************************************************
  */
#include "stm32f1xx_hal.h"
#include "My_OLED.h"
#include "mpu6050.h"
#include "key.h"
#include "oled_view.h"
#include "adc_brt.h"
#include "matrix7219.h"
#include "tetris.h"
#include "SEGGER_RTT.h"
#include <stdio.h>

/* SysTick 计时（stm32f1xx_it.c 中递增） */
volatile uint32_t g_millis = 0;

/* 时间分频节流（避免在主循环内做 mod 计数） */
static uint32_t s_last_mpu_tick = 0;   // 上次 MPU6050 采样
static uint32_t s_last_rtt_tick = 0;   // 上次 RTT 打印

/* SysTick 配置：72MHz → 1ms 周期 → 重装载值 72000-1 */
static void SystemClock_72MHz(void);
static void SysTick_Init_1ms(void);

int main(void)
{
    HAL_Init();
    SystemClock_72MHz();
    SysTick_Init_1ms();

    /* RTT 启动日志 */
    SEGGER_RTT_Init();
    SEGGER_RTT_WriteString(0, "[BOOT] stm32f103-mpu6050-oled v0.1\n");

    /* 硬件初始化 */
    if (MPU6050_Init() != 0)
    {
        SEGGER_RTT_WriteString(0, "[BOOT] MPU6050 init FAIL\n");
    }
    else
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "[BOOT] MPU6050 OK @ 0x%02X\n", MPU6050_GetAddress());
        SEGGER_RTT_WriteString(0, buf);
    }
    Key_Init();
    AdcBrt_Init();
    OledView_Init();
    {
        /* 点阵自检：init 返回 SPI 发送状态；display test 全亮 0.5s 验证模块供电/接线 */
        uint8_t mx_rc = Matrix7219_Init();
        char buf[64];
        snprintf(buf, sizeof(buf), "[BOOT] MAX7219 init rc=%u\n", (unsigned)mx_rc);
        SEGGER_RTT_WriteString(0, buf);
        Matrix7219_DisplayTest(1);
        SEGGER_RTT_WriteString(0, "[BOOT] MAX7219 display test ON (0.5s)\n");
        HAL_Delay(500);
        Matrix7219_DisplayTest(0);
        SEGGER_RTT_WriteString(0, "[BOOT] MAX7219 display test OFF\n");
    }
    SEGGER_RTT_WriteString(0, "[BOOT] PB15 long = recalibrate gyro\n");
    SEGGER_RTT_WriteString(0, "[BOOT] TETRIS page 3: tilt=move tap=rotate/restart\n");

    /* 出块播种：传感器噪声 + 上电时长（否则每次上电从同一序列开始） */
    {
        MPU6050_RawData_t sr;
        uint32_t seed = g_millis ^ 0x9E3779B9u;
        if (MPU6050_ReadRaw(&sr) == 0)
            seed ^= (uint32_t)(sr.ax * 1e6f) ^ ((uint32_t)(sr.ay * 1e6f) << 8)
                  ^ ((uint32_t)(sr.az * 1e6f) << 16);
        Tetris_Seed(seed);
    }
    /* 上电首次开局：必须显式 Reset——否则 s_rng 保持 BSS 零值，
     * LFSR 全零是不动点，出块永远是 0 号块（2026-10-26 真机"方块都一样"根因） */
    Tetris_Reset();

    /* 主循环 */
    while (1)
    {
        uint32_t now = g_millis;

        /* 按键 tick (1ms) */
        Key_Tick();

        /* KEY0 (PA0): 短按循环页 / 长按关屏 —— 语义不变 */
        KeyEvent_t ev0 = Key_GetEvent(KEY0);
        if (ev0 != KEY_EVENT_NONE)
        {
            OledView_HandleKey(ev0 == KEY_EVENT_SHORT, ev0 == KEY_EVENT_LONG);
            /* 屏开关与亮度联动：关屏期间只记档不发对比度，唤醒补发 */
            AdcBrt_ScreenOn(OledView_GetState() != OLEDS_OFF);
            /* RTT 记录按键事件（节流在 HandleKey 之外，避免事件丢失） */
            const char *evname = (ev0 == KEY_EVENT_SHORT) ? "SHORT" : "LONG";
            char buf[64];
            snprintf(buf, sizeof(buf), "[KEY] %s state=%d\n", evname, OledView_GetState());
            SEGGER_RTT_WriteString(0, buf);
        }

        /* KEY1 (PB15, 按下=低): 短按 = 方块旋转；长按 = 陀螺仪重校准 + 重开一局 */
        KeyEvent_t ev1 = Key_GetEvent(KEY1);
        if (ev1 == KEY_EVENT_SHORT)
        {
            Tetris_Rotate();
            SEGGER_RTT_WriteString(0, "[KEY] PB15 short: rotate\n");
        }
        else if (ev1 == KEY_EVENT_LONG)
        {
            char buf[64];
            /* 先刷屏再采样: 软 I2C 整帧 ~13ms 阻塞会污染校准采样 dt */
            OledView_ShowCalibHint(1);
            if (MPU6050_CalibrateGyroBias() == 0)
            {
                const MPU6050_RawData_t *rg = MPU6050_GetRaw();
                snprintf(buf, sizeof(buf), "[CAL] done gx=%.2f gy=%.2f gz=%.2f\n",
                         rg->gx, rg->gy, rg->gz);
            }
            else
            {
                snprintf(buf, sizeof(buf), "[CAL] FAIL\n");
            }
            SEGGER_RTT_WriteString(0, buf);
            OledView_ShowCalibHint(0);

            /* PB15 长按同时重开一局俄罗斯方块（手动重开入口） */
            Tetris_Reset();
            SEGGER_RTT_WriteString(0, "[KEY] PB15 long: tetris restarted\n");
        }

        /* 亮度电位器 (PA6/ADC1_IN6): 内部 50ms 节流评估 + 滞回三档 */
        AdcBrt_Poll();

        /* MPU6050 采样 (10ms / 100Hz) */
        if (now - s_last_mpu_tick >= 10)
        {
            s_last_mpu_tick = now;
            uint8_t mpu_rc = MPU6050_UpdateEuler(NULL);
            static uint8_t mpu_fail_logged = 0;
            if (mpu_rc != 0 && !mpu_fail_logged)
            {
                mpu_fail_logged = 1;
                SEGGER_RTT_WriteString(0, "[DIAG] MPU6050_UpdateEuler FAIL\n");
            }

            /* 俄罗斯方块 tick：跟随 100Hz 采样节拍（体感判定 + 重力下落） */
            if (mpu_rc == 0)
            {
                const MPU6050_RawData_t *raw = MPU6050_GetRaw();
                const MPU6050_Euler_t   *eul = MPU6050_GetEuler();
                Tetris_Tick(eul->pitch, eul->roll, raw->ax, raw->ay, raw->az);
                Matrix7219_ShowFrame(Tetris_GetFrame());
            }
        }

        /* OLED 显示 tick (100ms / 10Hz) */
        OledView_Tick();

        /* RTT 节流打印 (100ms) */
        if (now - s_last_rtt_tick >= 100)
        {
            s_last_rtt_tick = now;
            const MPU6050_RawData_t *r  = MPU6050_GetRaw();
            const MPU6050_Euler_t   *e  = MPU6050_GetEuler();
            char buf[128];
            snprintf(buf, sizeof(buf),
                "[STATE=%d last=%d] ax=%.2f ay=%.2f az=%.2f gx=%.1f gy=%.1f gz=%.1f roll=%.1f pitch=%.1f yaw=%.1f cur=%u next=%u\n",
                OledView_GetState(), 0,
                r->ax, r->ay, r->az, r->gx, r->gy, r->gz,
                e->roll, e->pitch, e->yaw,
                (unsigned)Tetris_GetCurrentShapeId(), (unsigned)Tetris_GetNextShapeId());
            SEGGER_RTT_WriteString(0, buf);
        }

        /* PA0 诊断：每 2s 打印一次电平（确认硬件极性） */
        static uint32_t s_last_diag_tick = 0;
        if (now - s_last_diag_tick >= 2000)
        {
            s_last_diag_tick = now;
            char buf[64];
            snprintf(buf, sizeof(buf), "[DIAG] PA0=%d (1=high=pressed)\n", Key_ReadLevel(KEY0));
            SEGGER_RTT_WriteString(0, buf);
        }
    }
}

/**
  * @brief  System Clock 配置：72MHz（外部 8MHz HSE × 9 倍频）
  */
static void SystemClock_72MHz(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    /* HSE 8MHz → PLL × 9 → 72MHz SYSCLK */
    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState            = RCC_HSE_ON;
    osc.HSEPredivValue      = RCC_HSE_PREDIV_DIV1;
    osc.PLL.PLLState        = RCC_PLL_ON;
    osc.PLL.PLLSource       = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLMUL          = RCC_PLL_MUL9;
    HAL_RCC_OscConfig(&osc);

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV2;     // APB1 36MHz（USART/I2C 上限）
    clk.APB2CLKDivider = RCC_HCLK_DIV1;     // APB2 72MHz（ADC/GPIO）
    HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_2);

    /* 使能 DWT（供软件 I2C 微秒延时使用） */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

/**
  * @brief  SysTick 配置 1ms 中断
  */
static void SysTick_Init_1ms(void)
{
    SysTick_Config(SystemCoreClock / 1000U);  // 72MHz / 1000 = 72000-1
    HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);
}
