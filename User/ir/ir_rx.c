/**
  ******************************************************************************
  * @file    ir_rx.c
  * @brief   PB6(EXTI6) 双边沿接收 + DWT 微秒时间戳
  *
  * @优先级   NVIC 1（SysTick 为 0）：毫秒计时与 HAL_Delay 不受影响
  * @回绕     CYCCNT 32 位 @72MHz 每 59.6s 回绕，全部用无符号差值
  * @队列     4 深度；主循环每轮取一次，OLED 整帧阻塞 13ms 也不会溢出
  ******************************************************************************
  */
#include "ir_rx.h"
#include "ir_decode.h"
#include "stm32f1xx_hal.h"

#define IR_PORT        GPIOB
#define IR_PIN         GPIO_PIN_6
#define IR_IRQn        EXTI9_5_IRQn
#define IR_NVIC_PRIO   1U
#define IR_Q_DEPTH     4U

static volatile IrEvent_t s_q[IR_Q_DEPTH];
static volatile uint8_t   s_head, s_tail;
static uint32_t           s_last_cyc;

void IrRx_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_AFIO_CLK_ENABLE();

    /* 接收头空闲输出高、收到 38kHz 载波时拉低；双边沿都要 */
    gpio.Pin  = IR_PIN;
    gpio.Mode = GPIO_MODE_IT_RISING_FALLING;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(IR_PORT, &gpio);

    HAL_NVIC_SetPriority(IR_IRQn, IR_NVIC_PRIO, 0);
    HAL_NVIC_EnableIRQ(IR_IRQn);

    IrDecode_Reset();
    s_head = 0;
    s_tail = 0;
    s_last_cyc = DWT->CYCCNT;
}

uint8_t IrRx_Poll(IrEvent_t *ev)
{
    if (s_head == s_tail) return 0;
    *ev = s_q[s_tail];
    s_tail = (uint8_t)((s_tail + 1U) % IR_Q_DEPTH);
    return 1;
}

/* HAL_GPIO_EXTI_IRQHandler 会清挂起位后回调这里 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    IrFrame_t f;
    uint32_t now, dt_us;
    uint8_t  level, next;

    if (GPIO_Pin != IR_PIN) return;

    now    = DWT->CYCCNT;
    dt_us  = (now - s_last_cyc) / (SystemCoreClock / 1000000U);
    s_last_cyc = now;
    level  = (HAL_GPIO_ReadPin(IR_PORT, IR_PIN) == GPIO_PIN_SET) ? 1U : 0U;

    if (!IrDecode_Edge(level, dt_us, &f)) return;

    next = (uint8_t)((s_head + 1U) % IR_Q_DEPTH);
    if (next == s_tail) return;          /* 队列满：丢新帧 */
    s_q[s_head].kind     = (f.kind == IR_FRAME_REPEAT) ? IR_EV_REPEAT : IR_EV_DATA;
    s_q[s_head].addr     = f.addr;
    s_q[s_head].cmd      = f.cmd;
    s_q[s_head].valid    = f.valid;
    s_q[s_head].standard = f.standard;
    s_q[s_head].raw      = f.raw;
    s_head = next;
}
