/**
  ******************************************************************************
  * @file    key.c
  * @brief   双键全功能非阻塞输入（移植自教程工程 01-全功能按键实现非阻塞式程序）
  *
  * @状态机（五态，20ms 扫描）
  *    IDLE ──按下──▶ PRESSED ──按住满 1s──▶ LONG（发 KEY_LONG，随后每 50ms 发 REPEAT）
  *    PRESSED ──释放──▶ RELEASED ──100ms 内无二次按下──▶ IDLE（发 KEY_SINGLE）
  *    RELEASED ──再次按下──▶ DOUBLE（发 KEY_DOUBLE）──释放──▶ IDLE
  *    LONG ──释放──▶ IDLE
  *
  * @与教程工程差异
  *    教程用"每次调用减 1"的计数值（假定严格 1ms 调用）；本工程主循环非严格
  *    节拍，改为 g_millis 绝对时间戳比较。标志位、状态迁移、时间常数不变。
  *    键位配置沿用本工程极性表（KEY0=PA0 高有效，KEY1=PB15 低有效）。
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
    K_IDLE = 0,
    K_PRESSED,
    K_RELEASED,
    K_DOUBLE,
    K_LONG,
} Key_State_t;

typedef struct {
    Key_State_t state;
    uint8_t     flags;      /* KEY_SINGLE/DOUBLE/LONG/REPEAT/HOLD/DOWN/UP */
    uint8_t     curr;       /* 本次扫描电平（1=按下） */
    uint8_t     prev;       /* 上次扫描电平 */
    uint32_t    timer_ms;   /* 当前状态超时时刻 */
} Key_Rt_t;

static Key_Rt_t s_rt[KEY_COUNT];
static uint32_t s_last_scan_ms;

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

        s_rt[i].state    = K_IDLE;
        s_rt[i].flags    = 0;
        s_rt[i].curr     = 0;
        s_rt[i].prev     = 0;
        s_rt[i].timer_ms = 0;
    }
    s_last_scan_ms = g_millis;
}

void Key_Tick(void)
{
    uint32_t now = g_millis;
    uint8_t i;

    if (now - s_last_scan_ms < KEY_SCAN_MS)
    {
        return;
    }
    s_last_scan_ms = now;

    for (i = 0; i < KEY_COUNT; i++)
    {
        Key_Rt_t *rt = &s_rt[i];

        rt->prev = rt->curr;
        rt->curr = key_raw_pressed((Key_Num)i);

        /* 电平类标志 */
        if (rt->curr) rt->flags |= KEY_HOLD;
        else          rt->flags &= (uint8_t)~KEY_HOLD;
        if (rt->curr && !rt->prev) rt->flags |= KEY_DOWN;
        if (!rt->curr && rt->prev) rt->flags |= KEY_UP;

        /* 状态机 */
        switch (rt->state)
        {
        case K_IDLE:
            if (rt->curr)
            {
                rt->timer_ms = now + KEY_TIME_LONG_MS;
                rt->state    = K_PRESSED;
            }
            break;

        case K_PRESSED:
            if (!rt->curr)                              /* 释放：进入双击窗口 */
            {
                rt->timer_ms = now + KEY_TIME_DOUBLE_MS;
                rt->state    = K_RELEASED;
            }
            else if (now >= rt->timer_ms)               /* 按住满 1s：发 LONG */
            {
                rt->flags   |= KEY_LONG;
                rt->timer_ms = now + KEY_TIME_REPEAT_MS;
                rt->state    = K_LONG;
            }
            break;

        case K_RELEASED:
            if (rt->curr)                               /* 窗口内再按：双击 */
            {
                rt->flags |= KEY_DOUBLE;
                rt->state  = K_DOUBLE;
            }
            else if (now >= rt->timer_ms)               /* 窗口超时：确认单击 */
            {
                rt->flags |= KEY_SINGLE;
                rt->state  = K_IDLE;
            }
            break;

        case K_DOUBLE:
            if (!rt->curr) rt->state = K_IDLE;
            break;

        case K_LONG:
            if (!rt->curr)                              /* 释放：回空闲 */
            {
                rt->state = K_IDLE;
            }
            else if (now >= rt->timer_ms)               /* 连发 */
            {
                rt->flags   |= KEY_REPEAT;
                rt->timer_ms = now + KEY_TIME_REPEAT_MS;
            }
            break;

        default:
            rt->state = K_IDLE;
            break;
        }
    }
}

uint8_t Key_Check(Key_Num keyNum, uint8_t Flag)
{
    if (keyNum >= KEY_COUNT) return 0;

    if (s_rt[keyNum].flags & Flag)
    {
        if (Flag != KEY_HOLD)
        {
            s_rt[keyNum].flags &= (uint8_t)~Flag;   /* 取走即清 */
        }
        return 1;
    }
    return 0;
}

uint8_t Key_ReadLevel(Key_Num keyNum)
{
    if (keyNum >= KEY_COUNT) return 0;
    return key_raw_pressed(keyNum);
}
