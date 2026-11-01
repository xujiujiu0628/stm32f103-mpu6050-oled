/**
  ******************************************************************************
  * @file    ir_rx.h
  * @brief   红外接收层：PB6 双边沿 EXTI + DWT 时间戳 + 事件队列
  *
  *  ISR 只做三件事：读 CYCCNT 算间隔 → 喂纯解码器 → 完整帧入队。
  *  动作派发一律留给主循环 IrRx_Poll()。
  ******************************************************************************
  */
#ifndef __IR_RX_H
#define __IR_RX_H

#include <stdint.h>

#define IR_EV_DATA    1
#define IR_EV_REPEAT  2

typedef struct {
    uint8_t  kind;      /* IR_EV_DATA / IR_EV_REPEAT */
    uint8_t  addr;
    uint8_t  cmd;
    uint8_t  valid;     /* 1 = 命令补码校验通过（动作门槛） */
    uint8_t  standard;  /* 1 = 地址补码也成立（扩展 NEC 时为 0） */
    uint32_t raw;       /* 原始 32 位，供 RTT 扫码/排障打印 */
} IrEvent_t;

void    IrRx_Init(void);
uint8_t IrRx_Poll(IrEvent_t *ev);

#endif /* __IR_RX_H */
