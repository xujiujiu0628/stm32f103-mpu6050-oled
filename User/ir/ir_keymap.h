/**
  ******************************************************************************
  * @file    ir_keymap.h
  * @brief   NEC 命令字节 → 功能映射（只匹配 cmd，不匹配 addr）
  *
  *  ⚠️ 默认值 = 常见 21 键 NEC 迷你遥控，未经实测。
  *  扫码校准：烧录后按一遍遥控器，从 RTT 的 [IR] addr=.. cmd=.. 行读值，
  *  只改本文件重编即完成适配，逻辑代码不动。
  ******************************************************************************
  */
#ifndef __IR_KEYMAP_H
#define __IR_KEYMAP_H

#define IR_KEY_LEFT      0x44U
#define IR_KEY_RIGHT     0x43U
#define IR_KEY_DOWN      0x07U
#define IR_KEY_ROTATE    0x15U
#define IR_KEY_HARDDROP  0x40U
#define IR_KEY_MODE      0x46U   /* 2026-11-18 真机扫码: 37 帧实测 */
#define IR_KEY_MATRIX_ONOFF 0x45U /* 2026-11-18 真机扫码: 70 帧实测 */

#endif /* __IR_KEYMAP_H */
