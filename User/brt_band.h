/**
  ******************************************************************************
  * @file    brt_band.h
  * @brief   亮度档位纯逻辑（滞回三档状态机）——无 HAL 依赖，host 可测
  *
  * 与 ADC 采样层（adc_brt.c）分离的理由：滞回算法是本项目唯一"边界即
  * 缺陷"的逻辑（阈值/带宽错一档肉眼可见但代码审查看不出），放在零依赖
  * 编译单元里用哨兵单测钉死（方法源自 adc-oled My_OLED 越界回写教训）。
  ******************************************************************************
  */
#ifndef BRT_BAND_H
#define BRT_BAND_H

#include <stdint.h>

typedef enum {
    BRT_BAND_LOW = 0,
    BRT_BAND_MID,
    BRT_BAND_HIGH,
    BRT_BAND_COUNT
} Brt_Band_t;

/* 阈值 (mV)。3.3V 系统, 上电默认 MID(屏出厂对比度)。滞回带 ±150mV ——
 * 电位器悬空/临界抖动不跨带。数值现场拧顺手再调即可, 不动架构 (FSD §6)。 */
#define BRT_MV_MID_ENTER   1150U   /* LOW→MID 上沿 */
#define BRT_MV_MID_EXIT_DN 1000U   /* MID→LOW 下沿（ENTER-150 滞回带） */
#define BRT_MV_MID_EXIT_UP 2650U   /* HIGH→MID 下沿 兼 MID→HIGH 下界参考 */
#define BRT_MV_HIGH_ENTER  2800U   /* →HIGH 上沿（两向唯一无条件越级域） */

/* SSD1306 0x81 对比度值, 一一对应三档 */
#define BRT_CONTRAST_LOW   0x40U
#define BRT_CONTRAST_MID   0x8EU   /* 屏默认对比度 */
#define BRT_CONTRAST_HIGH  0xFFU

/**
 * @brief  滞回档位状态机（纯函数）
 * @param  mv        本次平均电压 (mV)
 * @param  cur       当前档位（合法枚举值）
 * @retval 下一档位（可能等于 cur）
 *
 * 规则（滞回双沿 + 上越级单向，实现真值即此四条，测试逐条钉）：
 *   1. mv >= HIGH_ENTER(2800) → HIGH，无条件（唯一允许越级的方向——
 *      "用户快拧到底"是真实场景，LOW 态直入 HIGH 正确）
 *   2. cur HIGH: mv < 2650 → MID（回落逐档走，0mV 也只先落 MID，
 *      下一拍再落 LOW——防一根线松了瞬间从最亮跳最暗的惊吓）
 *   3. cur MID:  mv <= 1000 → LOW; mv >= 2650 → HIGH; 其间保持 MID
 *   4. cur LOW:  mv >= 1150 → MID
 * 滞回带 150mV 只作用于边界附近；2/3 的不对称（越级升/逐档降）是有意
 * 设计，登记于 FSD §6 FR-BRT-02。
 */
Brt_Band_t AdcBrt_NextBand(uint16_t mv, Brt_Band_t cur);

/* 档位 → 对比度查表 */
uint8_t AdcBrt_ContrastOf(Brt_Band_t band);

#endif /* BRT_BAND_H */
