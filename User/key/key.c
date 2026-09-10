/**
  ******************************************************************************
  * @file    key.c
  * @brief   PA0 用户按键（高电平触发，非阻塞消抖 + 长短按识别）
  *
  * @状态机（v2：时间窗消抖，抗毛刺）
  *    K_IDLE ──lvl=1──▶ K_ARM_RISING ──稳定50ms──▶ K_PRESSED
  *    K_ARM_RISING ──lvl=0（毛刺）──▶ K_IDLE
  *    K_PRESSED ──lvl=0──▶ K_ARM_FALLING ──稳定50ms──▶ K_IDLE（发 SHORT）
  *    K_ARM_FALLING ──lvl=1（毛刺）──▶ K_PRESSED（长按计时不中断）
  *    K_PRESSED ──按住≥1000ms──▶ 发 LONG（一次性）
  *
  * @v1 → v2 修复（2026-09-06 真机实测）：
  *    v1 的"消抖"只有一个 tick 判据（K_DEBOUNCING_DOWN 下个 tick 即进 PRESSED），
  *    KEY_DEBOUNCE_MS 定义了但从未生效 → PA0 毛刺导致状态机在"按下抑制"和
  *    "重新计时"间振荡，每凑满 1s 反复 fire LONG（5s 采集 ~25 次误触发）。
  *    v2 引入时间窗：电平变化后须连续稳定 KEY_DEBOUNCE_MS 才确认按下/释放；
  *    PRESSED 内短毛刺进 ARM_FALLING 不中断长按计时，真正抗干扰。
  *
  * @注意    PA0 极性：高电平 = 按下（内部下拉，见 ADR-002）
  ******************************************************************************
  */
#include "key.h"
#include "stm32f1xx_hal.h"

/* 外部 SysTick 计时 */
extern volatile uint32_t g_millis;

#define KEY_GPIO_PORT   GPIOA
#define KEY_GPIO_PIN    GPIO_PIN_0

typedef enum {
    K_IDLE = 0,        // 稳定未按下
    K_ARM_RISING,      // 检测到 1，等 50ms 稳定确认按下
    K_PRESSED,         // 已确认按下，计时长短按
    K_ARM_FALLING,     // 检测到掉 0，等 50ms 稳定确认释放
} Key_State_t;

static Key_State_t  s_state      = K_IDLE;
static uint32_t     s_change_ms  = 0;      // 最近一次电平变化时间
static uint32_t     s_press_ms   = 0;      // 确认按下时刻（长按计时起点）
static uint8_t      s_long_fired = 0;      // 长按事件是否已发（防止重复）
static KeyEvent_t   s_pending    = KEY_EVENT_NONE;

/**
 * @brief  读 PA0 当前电平（1=按下，0=未按下）
 */
static uint8_t read_level(void)
{
    return (HAL_GPIO_ReadPin(KEY_GPIO_PORT, KEY_GPIO_PIN) == GPIO_PIN_SET) ? 1 : 0;
}

void Key_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();

    gpio.Pin   = KEY_GPIO_PIN;
    gpio.Mode  = GPIO_MODE_INPUT;          // 纯输入（内部下拉）
    gpio.Pull  = GPIO_PULLDOWN;            // 关键：PA0 高电平触发，必须下拉
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(KEY_GPIO_PORT, &gpio);

    s_state      = K_IDLE;
    s_pending    = KEY_EVENT_NONE;
    s_long_fired = 0;
    s_press_ms   = 0;
    s_change_ms  = 0;
}

void Key_Tick(void)
{
    uint8_t  lvl = read_level();
    uint32_t now = g_millis;

    switch (s_state)
    {
    case K_IDLE:
        if (lvl == 1)
        {
            s_state     = K_ARM_RISING;
            s_change_ms = now;
        }
        break;

    case K_ARM_RISING:                // 等 1 电平稳定 ≥50ms 才确认按下
        if (lvl == 0)
        {
            s_state = K_IDLE;         // 又掉 0：毛刺，回退
        }
        else if (now - s_change_ms >= KEY_DEBOUNCE_MS)
        {
            s_state      = K_PRESSED;
            s_press_ms   = now;
            s_long_fired = 0;
        }
        break;

    case K_PRESSED:
        if (lvl == 0)
        {
            s_state     = K_ARM_FALLING;
            s_change_ms = now;
        }
        else if (!s_long_fired && (now - s_press_ms) >= KEY_LONG_MS)
        {
            s_long_fired = 1;
            s_pending    = KEY_EVENT_LONG;   // 按住 1s：立即发 LONG（不等释放）
        }
        break;

    case K_ARM_FALLING:               // 等 0 电平稳定 ≥50ms 才确认释放
        if (lvl == 1)
        {
            s_state = K_PRESSED;      // 毛刺回 1：未真正松开，长按计时不中断
        }
        else if (now - s_change_ms >= KEY_DEBOUNCE_MS)
        {
            if (!s_long_fired)
            {
                s_pending = KEY_EVENT_SHORT;
            }
            s_state = K_IDLE;
        }
        break;
    }
}

KeyEvent_t Key_GetEvent(void)
{
    KeyEvent_t ev = s_pending;
    s_pending = KEY_EVENT_NONE;
    return ev;
}

uint8_t Key_ReadLevel(void)
{
    return read_level();
}