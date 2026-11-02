/**
  ******************************************************************************
  * @file    ir_decode.h
  * @brief   NEC 红外解码器（纯 C，无 HAL / 无硬件依赖，host 可测）
  *
  *  输入：接收头输出的边沿（电平 + 距上一沿的微秒数）
  *  输出：解出的 32 位帧 / 重复帧
  ******************************************************************************
  */
#ifndef __IR_DECODE_H
#define __IR_DECODE_H

#include <stdint.h>

/* 帧种类 */
#define IR_FRAME_DATA    1
#define IR_FRAME_REPEAT  2

typedef struct {
    uint32_t raw;      /* 原始 32 位，bit0 = 第一个收到的位（NEC LSB 先发） */
    uint8_t  addr;     /* raw[7:0]   */
    uint8_t  cmd;      /* raw[23:16] */
    uint8_t  valid;    /* 1 = 命令补码校验通过（动作门槛） */
    uint8_t  standard; /* 1 = 地址补码也成立（标准 NEC） */
    uint8_t  kind;     /* IR_FRAME_DATA / IR_FRAME_REPEAT */
} IrFrame_t;

/**
 * @brief  复位解码器（含"上一帧"记录，重复帧窗口一并清空）
 */
void IrDecode_Reset(void);

/**
 * @brief  喂一个边沿
 * @param  level  边沿后的电平（0=低 1=高）
 * @param  dt_us  距上一沿的微秒数
 * @param  out    产出帧写入处（返回 1 时有效）
 * @retval 1=产出一帧 0=无输出
 */
uint8_t IrDecode_Edge(uint8_t level, uint32_t dt_us, IrFrame_t *out);

#endif /* __IR_DECODE_H */
