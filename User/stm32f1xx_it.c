/**
  ******************************************************************************
  * @file    stm32f1xx_it.c
  * @brief   中断服务例程：SysTick 1ms 计时 + 其他 HAL 默认 ISR 桩
  ******************************************************************************
  */
#include "stm32f1xx_hal.h"
#include "stm32f1xx_it.h"

/* SysTick 计时（main.c 中声明） */
extern volatile uint32_t g_millis;

/**
  * @brief  SysTick 中断：1ms 递增全局计时器
  */
void SysTick_Handler(void)
{
    g_millis++;
    HAL_IncTick();   // HAL 内部 tick（HAL_Delay 依赖）
}

/* ----------- 以下为 HAL 库要求的 ISR 桩（即使不用也需定义） ----------- */

void NMI_Handler(void)              { }
void HardFault_Handler(void)        { while (1) {} }
void MemManage_Handler(void)        { while (1) {} }
void BusFault_Handler(void)         { while (1) {} }
void UsageFault_Handler(void)       { while (1) {} }
void SVC_Handler(void)              { }
void DebugMon_Handler(void)         { }
void PendSV_Handler(void)           { }
