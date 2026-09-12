/**
  ******************************************************************************
  * @file    key.c
  * @brief   双键非阻塞输入（每键独立 v2 状态机 + 极性表驱动）
  *
  * @状态机（v2：时间窗消抖，2026-09-06 真机实测版）
  *    K_IDLE ──pressed──▶ K_ARM_RISING ──稳定50ms──▶ K_PRESSED
  *    K_ARM_RISING ──released（毛刺）──▶ K_IDLE
  *    K_PRESSED ──released──▶ K_ARM_FALLING ──稳定50ms──▶ K_IDLE（发 SHORT）
  *    K_ARM_FALLING ──pressed（毛刺）──▶ K_PRESSED（长按计时不中断）
  *    K_PRESSED ──按住≥1000ms──▶ 发 LONG（一次性）
  *
  * @多键化（2026-09-12，需求：PB15 长按触发陀螺仪重校准）
  *    单键结构泛化为 cfg[] 表 + per-key 状态数组；"按下"判定经极性宏折算
  *    （press_level），01-全功能按键工程的 Key_Num/状态机思想、弃其
  *    双击/连发（本项目无需求，YAGNI——登记于 FSD §6）。
  *    事件信箱 per-key 独立：两键同时操作不互相吞事件。
  ******************************************************************************
  */
#include "key.h"
#include "stm32f1xx_hal.h"

/* 外部 SysTick 计时 */
extern volatile uint32_t g_millis;

typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;
    uint8_t       press_level;   /* 1=高电平按下(PULLDOWN) 0=低电平按下(PULLUP) */
} Key_Cfg_t;

static const Key_Cfg_t s_cfg[KEY_COUNT] = {
    { GPIOA, GPIO_PIN_0, 1 },   /* KEY0: PA0  按下=高 */
    { GPIOB, GPIO_PIN_15, 0 },  /* KEY1: PB15 按下=低 */
};

typedef enum {
    K_IDLE = 0,        // 稳定未按下
    K_ARM_RISING,      // 检测到按下电平，等 50ms 稳定确认
    K_PRESSED,         // 已确认按下，计时长短按
    K_ARM_FALLING,     // 检测到释放电平，等 50ms 稳定确认
} Key_State_t;

typedef struct {
    Key_State_t state;
    uint32_t    change_ms;      // 最近一次电平变化时间
    uint32_t    press_ms;       // 确认按下时刻（长按计时起点）
    uint8_t     long_fired;     // 长按事件已发（防重复）
    KeyEvent_t  pending;        // 事件信箱（取走即清）
} Key_Rt_t;

static Key_Rt_t s_rt[KEY_COUNT];

/**
 * @brief  读指定键按下状态（1=按下，极性已折算）
 */
static uint8_t key_raw_pressed(Key_Num k)
{
    uint8_t lvl = (HAL_GPIO_ReadPin(s_cfg[k].port, s_cfg[k].pin) == GPIO_PIN_SET) ? 1 : 0;
    return (lvl == s_cfg[k].press_level) ? 1 : 0;
}

void Key_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    uint8_t i;

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    for (i = 0; i < KEY_COUNT; i++)
    {
        gpio.Pin   = s_cfg[i].pin;
        gpio.Mode  = GPIO_MODE_INPUT;
        gpio.Pull  = s_cfg[i].press_level ? GPIO_PULLDOWN : GPIO_PULLUP;
        gpio.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(s_cfg[i].port, &gpio);

        s_rt[i].state      = K_IDLE;
        s_rt[i].pending    = KEY_EVENT_NONE;
        s_rt[i].long_fired = 0;
        s_rt[i].press_ms   = 0;
        s_rt[i].change_ms  = 0;
    }
}

void Key_Tick(void)
{
    uint32_t now = g_millis;
    uint8_t i;

    for (i = 0; i < KEY_COUNT; i++)
    {
        Key_Rt_t *rt = &s_rt[i];
        uint8_t pressed = key_raw_pressed((Key_Num)i);

        switch (rt->state)
        {
        case K_IDLE:
            if (pressed)
            {
                rt->state     = K_ARM_RISING;
                rt->change_ms = now;
            }
            break;

        case K_ARM_RISING:              // 等按下电平稳定 ≥50ms 才确认
            if (!pressed)
            {
                rt->state = K_IDLE;     // 毛刺，回退
            }
            else if (now - rt->change_ms >= KEY_DEBOUNCE_MS)
            {
                rt->state      = K_PRESSED;
                rt->press_ms   = now;
                rt->long_fired = 0;
            }
            break;

        case K_PRESSED:
            if (!pressed)
            {
                rt->state     = K_ARM_FALLING;
                rt->change_ms = now;
            }
            else if (!rt->long_fired && (now - rt->press_ms) >= KEY_LONG_MS)
            {
                rt->long_fired = 1;
                rt->pending    = KEY_EVENT_LONG;  // 按住 1s：立即发 LONG
            }
            break;

        case K_ARM_FALLING:             // 等释放电平稳定 ≥50ms 才确认
            if (pressed)
            {
                rt->state = K_PRESSED;  // 毛刺回按下：长按计时不中断
            }
            else if (now - rt->change_ms >= KEY_DEBOUNCE_MS)
            {
                if (!rt->long_fired)
                {
                    rt->pending = KEY_EVENT_SHORT;
                }
                rt->state = K_IDLE;
            }
            break;
        }
    }
}

KeyEvent_t Key_GetEvent(Key_Num keyNum)
{
    if (keyNum >= KEY_COUNT)
    {
        return KEY_EVENT_NONE;
    }
    KeyEvent_t ev = s_rt[keyNum].pending;
    s_rt[keyNum].pending = KEY_EVENT_NONE;
    return ev;
}

uint8_t Key_ReadLevel(Key_Num keyNum)
{
    if (keyNum >= KEY_COUNT)
    {
        return 0;
    }
    return key_raw_pressed(keyNum);
}
