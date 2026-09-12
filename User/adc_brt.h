/**
  ******************************************************************************
  * @file    adc_brt.h
  * @brief   电位器亮度控制模块（ADC1_IN6 / PA6，滞回三档）
  ******************************************************************************
  */
#ifndef ADC_BRT_H
#define ADC_BRT_H

#include <stdint.h>
#include "brt_band.h"

void        AdcBrt_Init(void);
/* 主循环每圈调用：内部按 g_millis 节流采样+判决（50ms 评估一拍） */
void        AdcBrt_Poll(void);
Brt_Band_t  AdcBrt_Band(void);          /* 当前档位（供显示/UI） */
uint16_t    AdcBrt_Millivolts(void);    /* 最近一次滑动平均 mV    */
/* 屏开关联动：关屏时挂起对比度下发，唤醒后强制补发当前档 */
void        AdcBrt_ScreenOn(uint8_t on);

#endif /* ADC_BRT_H */
