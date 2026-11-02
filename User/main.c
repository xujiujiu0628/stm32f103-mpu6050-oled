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
#include "ir_rx.h"
#include "ir_keymap.h"
#include "SEGGER_RTT.h"
#include <stdio.h>

/* SysTick 计时（stm32f1xx_it.c 中递增） */
volatile uint32_t g_millis = 0;

/* 时间分频节流（避免在主循环内做 mod 计数） */
static uint32_t s_last_mpu_tick = 0;   // 上次 MPU6050 采样
static uint32_t s_last_rtt_tick = 0;   // 上次 RTT 打印

/* 控制模式：0=SENSOR(MPU 体感) 1=IR(遥控器全接管)，上电默认体感 */
static uint8_t s_ctrl_mode = TETRIS_INPUT_SENSOR;

/* ---------------- 第 1 步实测：刷帧耗时 / 实际采样周期 / 循环节拍 ----------------
 * 1s 一行 RTT 心跳，整数定点打印（nano printf 不认 %f，历史坑）。
 * PERF_TRACE=0 可整体关掉（探针只读全局变量，关掉后无功能影响）。 */
#define PERF_TRACE 1

#if PERF_TRACE
static uint32_t s_perf_t0       = 0;   /* 统计窗起点 */
static uint32_t s_perf_loops    = 0;   /* 主循环迭代数 */
static uint32_t s_perf_free     = 0;   /* 迭代内没有发生刷屏的次数（"轻迭代"） */
static uint32_t s_perf_mpu_n    = 0;   /* 采样次数 */
static uint32_t s_perf_mpu_sum  = 0;   /* 实际采样间隔累计 (ms) */
static uint32_t s_perf_mpu_max  = 0;   /* 实际采样间隔最大值 (ms) */
static uint32_t s_perf_mpu_late = 0;   /* 间隔 >15ms（漏掉至少一拍）次数 */
static uint32_t s_perf_fmax_us  = 0;   /* 窗口内最坏一帧刷屏耗时 (us) */
static uint32_t s_perf_f0       = 0;   /* 窗口起点帧计数 */

/* 实际采样间隔：喂给互补滤波 dt 的真值，名义应为 10ms */
static void perf_mpu_interval(uint32_t interval_ms)
{
    s_perf_mpu_n++;
    s_perf_mpu_sum += interval_ms;
    if (interval_ms > s_perf_mpu_max) s_perf_mpu_max = interval_ms;
    if (interval_ms > 15U)            s_perf_mpu_late++;
}

static void perf_tick(uint32_t now)
{
    char buf[176];
    uint32_t win_ms, frames, avg_ms10;

    if (now - s_perf_t0 < 1000U) return;

    win_ms   = now - s_perf_t0;
    frames   = g_perf_frames - s_perf_f0;
    avg_ms10 = s_perf_mpu_n ? (s_perf_mpu_sum * 10U / s_perf_mpu_n) : 0U;

    snprintf(buf, sizeof(buf),
        "[PERF] flush_us=%lu max_us=%lu draw_us=%lu fps=%lu | "
        "mpu n=%lu avg=%lu.%lums max_ms=%lu late=%lu | loop=%lu/s free=%lu\n",
        (unsigned long)g_perf_flush_us, (unsigned long)s_perf_fmax_us,
        (unsigned long)g_perf_draw_us,  (unsigned long)(frames * 1000U / win_ms),
        (unsigned long)s_perf_mpu_n,
        (unsigned long)(avg_ms10 / 10U), (unsigned long)(avg_ms10 % 10U),
        (unsigned long)s_perf_mpu_max,   (unsigned long)s_perf_mpu_late,
        (unsigned long)(s_perf_loops * 1000U / win_ms), (unsigned long)s_perf_free);
    SEGGER_RTT_WriteString(0, buf);

    s_perf_t0 = now;
    s_perf_loops = 0; s_perf_free = 0;
    s_perf_mpu_n = 0; s_perf_mpu_sum = 0; s_perf_mpu_max = 0; s_perf_mpu_late = 0;
    s_perf_fmax_us = 0; s_perf_f0 = g_perf_frames;
}
#endif

/* SysTick 配置：72MHz → 1ms 周期 → 重装载值 72000-1 */
static void SystemClock_72MHz(void);
static void SysTick_Init_1ms(void);

/**
  * @brief  红外事件派发（主循环调用；ISR 只入队，不做动作）
  * @note   MODE 键两种模式都生效；其余动作键仅 IR 模式生效；
  *         重复帧只驱动左/右/软降，旋转与硬降忽略重复帧
  */
static void ir_dispatch(void)
{
    IrEvent_t ev;
    char buf[80];

    while (IrRx_Poll(&ev))
    {
        if (ev.kind == IR_EV_DATA)
        {
            snprintf(buf, sizeof(buf), "[IR] addr=0x%02X cmd=0x%02X raw=0x%08lX%s\n",
                     (unsigned)ev.addr, (unsigned)ev.cmd, (unsigned long)ev.raw,
                     ev.valid ? "" : " BAD");
            SEGGER_RTT_WriteString(0, buf);
        }
        if (!ev.valid) continue;                 /* 命令补码不过 → 不动作 */

        if (ev.cmd == IR_KEY_MODE)
        {
            if (ev.kind == IR_EV_REPEAT) continue;
            s_ctrl_mode = (s_ctrl_mode == TETRIS_INPUT_SENSOR)
                          ? (uint8_t)TETRIS_INPUT_IR : (uint8_t)TETRIS_INPUT_SENSOR;
            Tetris_SetInputMode((TetrisInputMode)s_ctrl_mode);
            SEGGER_RTT_WriteString(0, s_ctrl_mode ? "[IR] mode=IR\n" : "[IR] mode=MPU\n");
            continue;
        }

        if (s_ctrl_mode != TETRIS_INPUT_IR) continue;   /* MPU 模式下遥控动作键不生效 */

        switch (ev.cmd)
        {
        case IR_KEY_LEFT:
            Tetris_ShiftLeft();
            break;
        case IR_KEY_RIGHT:
            Tetris_ShiftRight();
            break;
        case IR_KEY_DOWN:
            Tetris_SoftDrop();
            break;
        case IR_KEY_ROTATE:
            if (ev.kind == IR_EV_REPEAT) break;
            Tetris_Rotate();
            SEGGER_RTT_WriteString(0, "[IR] rotate\n");
            break;
        case IR_KEY_HARDDROP:
            if (ev.kind == IR_EV_REPEAT) break;
            {
                uint8_t rows = Tetris_HardDrop();
                snprintf(buf, sizeof(buf), "[IR] harddrop rows=%u\n", (unsigned)rows);
                SEGGER_RTT_WriteString(0, buf);
            }
            break;
        default:
            break;
        }
    }
}

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
    IrRx_Init();
    SEGGER_RTT_WriteString(0, "[IR] init pin=PB12\n");
    SEGGER_RTT_WriteString(0, "[IR] mode=MPU\n");
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
    /* 自检 0.5s 期间 ISR 仍会入队：进主循环前清掉，避免上电误按在启动后补放 */
    IrRx_Flush();
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

#if PERF_TRACE
    s_perf_t0 = g_millis;
    s_perf_f0 = g_perf_frames;
#endif

    /* 主循环 */
    while (1)
    {
        uint32_t now = g_millis;
#if PERF_TRACE
        uint32_t perf_f0 = g_perf_frames;   /* 本迭代是否发生整帧刷 */
        s_perf_loops++;
#endif

        /* 按键 tick (1ms) */
        Key_Tick();

        /* 红外遥控事件派发（ISR 只入队，动作在这里做） */
        ir_dispatch();

        /* KEY0 (PA0): 单击循环页 / 长按关屏 —— 全功能键标志位接口 */
        {
            uint8_t k0_short = Key_Check(KEY0, KEY_SINGLE);
            uint8_t k0_long  = Key_Check(KEY0, KEY_LONG);
            if (k0_short || k0_long)
            {
                OledView_HandleKey(k0_short, k0_long);
                /* 屏开关与亮度联动：关屏期间只记档不发对比度，唤醒补发 */
                AdcBrt_ScreenOn(OledView_GetState() != OLEDS_OFF);
                {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "[KEY] %s state=%d\n",
                             k0_long ? "LONG" : "SHORT", OledView_GetState());
                    SEGGER_RTT_WriteString(0, buf);
                }
            }
        }

        /* KEY1 (PB15): 单击 = 方块旋转；长按 = 陀螺仪重校准 + 重开一局
         * （状态机互斥：长按触发后不会补发单击） */
        /* 按键按下即静音敲击检测 300ms：按键震动不能被当成"敲击旋转" */
        if (Key_Check(KEY1, KEY_DOWN) || Key_Check(KEY0, KEY_DOWN))
        {
            Tetris_MuteTap(300);
        }

        if (Key_Check(KEY1, KEY_SINGLE))
        {
            Tetris_Rotate();
            SEGGER_RTT_WriteString(0, "[KEY] PB15 short: rotate\n");
        }
        if (Key_Check(KEY1, KEY_LONG))
        {
            char buf[64];
            /* 校准 ~1s 内主循环不取队列：先清掉积压，避免校准后集中补放陈旧动作 */
            IrRx_Flush();
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
#if PERF_TRACE
            perf_mpu_interval(now - s_last_mpu_tick);   /* 真实间隔，含被刷屏推后的部分 */
#endif
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
#if PERF_TRACE
        if (g_perf_flush_us > s_perf_fmax_us) s_perf_fmax_us = g_perf_flush_us;
        if (g_perf_frames == perf_f0) s_perf_free++;       /* 本迭代没刷屏 = 轻迭代 */
        perf_tick(g_millis);
#endif
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
