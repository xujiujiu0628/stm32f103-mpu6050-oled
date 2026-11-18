/**
  ******************************************************************************
  * @file    oled_view.c
  * @brief   OLED 显示状态机：4 状态切换 + 10Hz 节流
  *
  * @状态机
  *    OLEDS_TETRIS(0) → OLEDS_RAW(1) → OLEDS_EULER(2) → OLEDS_ATTITUDE(3)
  *      → OLEDS_TETRIS(0)：短按循环（2026-11-18 用户要求: 俄罗斯方块由末页挪到首页）
  *    任意非 OFF 状态 + 长按 → OLEDS_OFF(4)；OFF + 短按 → 唤醒到 last_state
  *    OFF + 短按 → 唤醒到 last_state（保留关屏前的页面）
  *    OFF + 长按 → 无操作
  *
  * @显示
  *    页面 0：六轴（ax/ay/az + gx/gy/gz），8x6 字体 6 行
  *    页面 1：欧拉角（Roll/Pitch 数值，Yaw 显示 "live" 占位）
  *    页面 2：2D 姿态点图（中心十字 + 参考圆 + 实时点）
  *    页面 3：关屏（SSD1306 命令 0xAE）
  ******************************************************************************
  */
#include "oled_view.h"
#include "My_OLED.h"
#include "mpu6050.h"
#include "tetris.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

/* 外部 SysTick 计时 */
extern volatile uint32_t g_millis;

/* ---------------- 性能观测探针 + 刷屏策略（第 1/2 步） ----------------
 * DWT->CYCCNT 已在 main.c SystemClock_72MHz 里使能；72MHz → 1us = 72 拍。
 *
 * @2026-10-26 真机实测基线（整帧刷）：单次刷屏阻塞 131.2ms / 帧，主循环一个
 *   迭代只剩一次采样机会 → 实际采样率 7.3Hz（名义 100Hz），OLED 3.6s 才刷一帧。
 *   主循环 7 迭代/s、轻迭代 0 次：整条循环就是"采样 → 刷屏"串行链。
 *
 * @刷屏方式（OLEDS_SLICE_ENABLE 切换）
 *   0 = 整帧：My_OLED_Update 一次发 8 页 1024 字节 → 单次阻塞 ~131ms
 *   1 = 切片：每次只发 64 字节（半页），16 片凑一帧 → 单次阻塞 ~8ms（实测 8.4ms），
 *           采样节拍回到 96~97Hz。整帧总字节不变，但帧率从 7fps 降到 5~6fps：
 *           每片多一次定位事务，且片与片之间被采样/按键插队。
 */
#define OLEDS_SLICE_ENABLE   1
#define OLEDS_SLICE_BYTES    64U                          /* 一片字节数（64 = 半页） */
#define OLEDS_SLICE_PER_PAGE (128U / OLEDS_SLICE_BYTES)   /* 每页片数 */
#define OLEDS_SLICE_N        (8U * OLEDS_SLICE_PER_PAGE)  /* 整帧片数 */

volatile uint32_t g_perf_flush_us = 0;   /* 单次刷屏阻塞耗时（整帧版=整帧，切片版=一片） */
volatile uint32_t g_perf_draw_us  = 0;   /* 一次排版+绘制（纯 RAM 操作）耗时 */
volatile uint32_t g_perf_frames   = 0;   /* 累计刷完的整帧数 */

#if OLEDS_SLICE_ENABLE
static uint8_t s_flush_pending = 0;      /* 有帧待发 */
static uint8_t s_slice_idx     = 0;      /* 下一片片号 */
#endif

static uint32_t us_since(uint32_t t0)
{
    return (DWT->CYCCNT - t0) / (SystemCoreClock / 1000000U);
}

/* 周期性渲染路径的刷屏入口：切片模式只画不刷（由 Tick 的分片机推进），
 * 整帧模式立即整帧刷并计时 */
static void oled_flush(void)
{
#if OLEDS_SLICE_ENABLE
    /* no-op：扇出交给 OledView_Tick */
#else
    uint32_t t0 = DWT->CYCCNT;
    My_OLED_Update();
    g_perf_flush_us = us_since(t0);
    g_perf_frames++;
#endif
}

/* 整帧刷：启动画面 / 校准提示这类一次性路径专用
 * （成本一次性，且校准场景本就要求把刷屏成本花在采样之前，不计入 PERF 峰值） */
static void oled_flush_full(void)
{
    My_OLED_Update();
#if OLEDS_SLICE_ENABLE
    s_flush_pending = 0;   /* 缓冲已整帧发出，作废未发完的片 */
#endif
}

#if OLEDS_SLICE_ENABLE
/* 发一片；返回 1 表示整帧发完 */
static uint8_t oled_flush_slice(void)
{
    uint8_t  page = (uint8_t)(s_slice_idx / OLEDS_SLICE_PER_PAGE);
    uint8_t  col  = (uint8_t)((s_slice_idx % OLEDS_SLICE_PER_PAGE) * OLEDS_SLICE_BYTES);
    uint32_t t0   = DWT->CYCCNT;

    My_OLED_UpdateChunk(page, col, OLEDS_SLICE_BYTES);
    g_perf_flush_us = us_since(t0);

    if (++s_slice_idx >= OLEDS_SLICE_N)
    {
        s_slice_idx = 0;
        g_perf_frames++;
        return 1;
    }
    return 0;
}
#endif

static OledState_t s_state      = OLEDS_TETRIS;
static OledState_t s_last_state = OLEDS_RAW;  // 关屏前的状态（首次默认 RAW，唤醒后回这里）
static uint32_t    s_last_render_ms = 0;
static uint8_t     s_first_render = 0;          // 启动画面已显示标志
static uint8_t     s_calib_overlay = 0;         // 校准覆盖层在场（FR-CAL-02）

/* ---------------- 内部工具：数字格式化 ---------------- */

/**
 * @brief  格式化为 "ax:  +0.02 g" 这样的 21 字符串
 */
static void format_axis(char *buf, const char *label, float val, const char *unit)
{
    char sign = ' ';
    if (val < 0) { sign = '-'; val = -val; }
    else         { sign = '+'; }
    /* val 在 0~99 范围，2 位小数；足够 6 轴原始值 */
    int integer = (int)val;
    int frac    = (int)(val * 100.0f + 0.5f) % 100;
    sprintf(buf, "%s: %c%d.%02d %s", label, sign, integer, frac, unit);
}

/* ---------------- 4 个页面渲染 ---------------- */

static void render_raw(void)
{
    const MPU6050_RawData_t *r = MPU6050_GetRaw();
    char buf[24];
    My_OLED_Clear();
    /* 6 行 8x6 字体：y=8,16,24,40,48,56（32 行空 1 行做视觉分组） */
    format_axis(buf, "ax", r->ax, "g ");  My_OLED_ShowString(0,  8, buf, 6);
    format_axis(buf, "ay", r->ay, "g ");  My_OLED_ShowString(0, 16, buf, 6);
    format_axis(buf, "az", r->az, "g ");  My_OLED_ShowString(0, 24, buf, 6);
    format_axis(buf, "gx", r->gx, "d/s"); My_OLED_ShowString(0, 40, buf, 6);
    format_axis(buf, "gy", r->gy, "d/s"); My_OLED_ShowString(0, 48, buf, 6);
    format_axis(buf, "gz", r->gz, "d/s"); My_OLED_ShowString(0, 56, buf, 6);
    oled_flush();
}

static void render_euler(void)
{
    const MPU6050_Euler_t *e = MPU6050_GetEuler();
    char buf[24];
    My_OLED_Clear();
    /* 3 行大字（8x6 字体居中显示） */
    sprintf(buf, "Roll:%+6.1f", e->roll);   My_OLED_ShowString(0, 24, buf, 6);
    sprintf(buf, "Pitch:%+5.1f", e->pitch); My_OLED_ShowString(0, 40, buf, 6);
    My_OLED_ShowString(0, 56, "Yaw: live", 6);   // 文字占位
    oled_flush();
}

/* Bresenham 圆算法：画 8 分点对称圆（4 象限 × 2 对称轴） */
static void draw_circle(uint8_t cx, uint8_t cy, uint8_t r)
{
    int x = 0, y = r;
    int d = 3 - 2 * r;
    while (x <= y)
    {
        My_OLED_DrawPoint(cx + x, cy + y);
        My_OLED_DrawPoint(cx - x, cy + y);
        My_OLED_DrawPoint(cx + x, cy - y);
        My_OLED_DrawPoint(cx - x, cy - y);
        My_OLED_DrawPoint(cx + y, cy + x);
        My_OLED_DrawPoint(cx - y, cy + x);
        My_OLED_DrawPoint(cx + y, cy - x);
        My_OLED_DrawPoint(cx - y, cy - x);
        if (d < 0) d += 4 * x + 6;
        else      { d += 4 * (x - y) + 10; y--; }
        x++;
    }
}

static void render_attitude(void)
{
    const MPU6050_Euler_t *e = MPU6050_GetEuler();

    My_OLED_Clear();

    /* 中心十字（虚线：每 4 像素一划，让步于实时点区分） */
    for (uint8_t x = 0; x < 128; x += 4) { My_OLED_DrawPoint(x, 32); }
    for (uint8_t y = 0; y < 64;  y += 4) { My_OLED_DrawPoint(64, y); }

    /* 参考圆（半径 24 像素 = 67° 倾角警告边界） */
    draw_circle(64, 32, 24);

    /* 实时点：±90° 满量程 → ±32 像素（X 轴），±24 像素（Y 轴） */
    float roll  = e->roll;
    float pitch = e->pitch;
    /* 超量程：点消失（清屏时已经抹掉） */
    if (roll > -90.0f && roll < 90.0f && pitch > -90.0f && pitch < 90.0f)
    {
        int16_t px = 64 + (int16_t)(roll  * 32.0f / 90.0f);
        /* Y 轴翻转：板子前倾（pitch>0）→ 点向上（y 减小） */
        int16_t py = 32 - (int16_t)(pitch * 24.0f / 90.0f);
        if (px >= 1 && px <= 126 && py >= 1 && py <= 62)
        {
            /* 3x3 十字标记 */
            for (int dx = -1; dx <= 1; dx++) My_OLED_DrawPoint(px + dx, py);
            for (int dy = -1; dy <= 1; dy++) My_OLED_DrawPoint(px, py + dy);
        }
    }

    oled_flush();
}

/* ---------------- 关屏/开屏薄封装 ---------------- */

/* 页面 3：俄罗斯方块副屏 —— 下一块预览 + 分数/行数/速度 */
static void render_tetris(void)
{
    char buf[24];
    My_OLED_Clear();
    My_OLED_ShowString(0, 0, (char *)"TETRIS", 6);

    /* 下一块预览：8x8 帧取 4x4 有效区放大到 OLED 上 16x16 区域（每点 2x2） */
    {
        const uint8_t *nf = Tetris_GetNextShape();
        uint8_t by, bx;
        for (by = 0; by < 8; by++)
            for (bx = 0; bx < 8; bx++)
                if ((nf[by] >> (7 - bx)) & 1)
                {
                    /* 画 2x2 实心块：(76+2bx, 16+2by) 起，屏幕右半区 */
                    My_OLED_DrawPoint(76 + 2 * bx,     16 + 2 * by);
                    My_OLED_DrawPoint(76 + 2 * bx + 1, 16 + 2 * by);
                    My_OLED_DrawPoint(76 + 2 * bx,     16 + 2 * by + 1);
                    My_OLED_DrawPoint(76 + 2 * bx + 1, 16 + 2 * by + 1);
                }
    }
    My_OLED_ShowString(64, 8, (char *)"NEXT", 6);

    sprintf(buf, "Score:%u", (unsigned)Tetris_GetScore());
    My_OLED_ShowString(0, 24, buf, 6);
    sprintf(buf, "Lines:%u", (unsigned)Tetris_GetLines());
    My_OLED_ShowString(0, 40, buf, 6);

    if (Tetris_IsGameOver())
        My_OLED_ShowString(0, 56, (char *)"NEW GAME...", 6);
    else if (Tetris_GetInputMode() == TETRIS_INPUT_IR)
        My_OLED_ShowString(0, 56, (char *)"CTRL:IR  REMOTE", 6);
    else
        My_OLED_ShowString(0, 56, (char *)"CTRL:MPU TILT+TAP", 6);

    oled_flush();
}

void OledView_DisplayOff(void)
{
    My_OLED_WriteCommand(0xAE);   // SSD1306 关显示
}

void OledView_DisplayOn(void)
{
    My_OLED_WriteCommand(0xAF);   // SSD1306 开显示
}

/* ---------------- 校准提示覆盖层 (FSD §6 FR-CAL-02) ---------------- */

/**
 * @brief  hint=1: 全屏覆盖 "CALIBRATING... Keep still"（调用方随后执行
 *         阻塞校准——本函数内部完成整帧刷, 把 13ms 刷屏成本花在校准之前,
 *         不污染校准采样 dt, 09-06 教训）。
 *         hint=0: 撤销覆盖层（ForceRefresh, 下一 tick 重绘当前页）。
 *         关屏状态下发 hint=1 会被忽略（屏黑着提示无意义）。
 */
void OledView_ShowCalibHint(uint8_t hint)
{
    if (hint)
    {
        if (s_state == OLEDS_OFF)
        {
            return;
        }
        My_OLED_Clear();
        My_OLED_ShowString(0, 24, (char *)"CALIBRATING...", 6);
        My_OLED_ShowString(0, 40, (char *)"Keep still ~1s", 6);
        oled_flush_full();          /* 校准前整帧刷：成本一次性，且不能污染采样 dt */
        s_calib_overlay = 1;
    }
    else if (s_calib_overlay)
    {
        s_calib_overlay = 0;
        OledView_ForceRefresh();
    }
}

/* ---------------- 启动画面 ---------------- */

static void render_startup(void)
{
    My_OLED_Clear();
    MPU6050_State_t st = MPU6050_GetState();
    if (st == MPU6050_STATE_OK)
    {
        char buf[24];
        sprintf(buf, "MPU6050 OK@0x%02X", MPU6050_GetAddress());
        My_OLED_ShowString(0, 24, buf, 6);
        My_OLED_ShowString(0, 40, "  Ready", 6);
    }
    else
    {
        My_OLED_ShowString(0, 24, "MPU6050 FAIL", 6);
        My_OLED_ShowString(0, 40, "  Check HW", 6);
    }
    oled_flush_full();              /* 启动画面一次性整帧刷 */
    s_first_render = 1;
}

/* ---------------- 公开 API ---------------- */

void OledView_Init(void)
{
    My_OLED_Init();
    render_startup();
    s_state = OLEDS_TETRIS;
    s_last_state = OLEDS_RAW;
    s_last_render_ms = g_millis;
    s_first_render = 0;
}

void OledView_ForceRefresh(void)
{
    s_last_render_ms = 0;  // 下次 tick 立即渲染
}

void OledView_HandleKey(uint8_t short_press, uint8_t long_press)
{
    if (s_state == OLEDS_OFF)
    {
        /* 关屏状态：短按唤醒到 last_state；长按无操作 */
        if (short_press)
        {
            s_state = s_last_state;
            OledView_DisplayOn();
            OledView_ForceRefresh();
        }
        return;
    }

    /* 非关屏状态：长按 → 关屏 */
    if (long_press)
    {
        s_last_state = s_state;   // 记住关屏前的状态
        s_state = OLEDS_OFF;
        OledView_DisplayOff();
        return;
    }

    /* 短按：在 0..3 四页间循环，4=OFF 不在循环内 */
    if (short_press)
    {
        s_state = (OledState_t)((s_state + 1) % OLEDS_OFF);
        OledView_ForceRefresh();
    }
}

void OledView_Tick(void)
{
    /* 启动画面：显示 1s 后切到第一页 */
    if (!s_first_render)
    {
        if (g_millis - s_last_render_ms >= 1000)
        {
            s_state = OLEDS_TETRIS;
            s_last_render_ms = g_millis;
            render_tetris();
            s_first_render = 1;
        }
        return;
    }

    /* 关屏状态：什么都不渲染（未发完的帧一并作废） */
    if (s_state == OLEDS_OFF)
    {
#if OLEDS_SLICE_ENABLE
        s_flush_pending = 0;
#endif
        return;
    }

#if OLEDS_SLICE_ENABLE
    /* 有未发完的帧：本迭代只发一片就返回，把整帧阻塞摊到 OLEDS_SLICE_N 次主循环，
     * 中间的主循环迭代照常跑采样/按键/点阵 */
    if (s_flush_pending)
    {
        if (oled_flush_slice()) s_flush_pending = 0;
        return;
    }
#endif

    /* 10Hz 节流 */
    if (g_millis - s_last_render_ms < OLEDS_REFRESH_MS) return;
    s_last_render_ms = g_millis;

    {
        uint32_t t_render = DWT->CYCCNT;
        switch (s_state)
        {
        case OLEDS_RAW:      render_raw();      break;
        case OLEDS_EULER:    render_euler();    break;
        case OLEDS_ATTITUDE: render_attitude(); break;
        case OLEDS_TETRIS:   render_tetris();   break;
        default: break;
        }
#if OLEDS_SLICE_ENABLE
        g_perf_draw_us  = us_since(t_render);   /* 纯排版+绘制（刷屏已外移） */
        s_slice_idx     = 0;
        s_flush_pending = 1;                    /* 下一迭代开始逐片发送 */
#else
        {
            uint32_t total = us_since(t_render);        /* 绘制 + 整帧刷 */
            g_perf_draw_us = (total > g_perf_flush_us) ? (total - g_perf_flush_us) : 0U;
        }
#endif
    }
}

OledState_t OledView_GetState(void) { return s_state; }
