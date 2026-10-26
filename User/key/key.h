/**
  ******************************************************************************
  * @file    key.h
  * @brief   双键全功能非阻塞输入（移植自桌面教程工程 01-全功能按键实现非阻塞式程序）
  *
  *  事件标志位（Key_Check 取走即清，KEY_HOLD 为电平不清）：
  *    KEY_SINGLE  单击（释放后 100ms 无二次按下确认）
  *    KEY_DOUBLE  双击（单击确认前再次按下，按下沿即上报）
  *    KEY_LONG    长按（按住满 1000ms，按下期间立即上报一次）
  *    KEY_REPEAT  长按连发（进入长按后每 50ms 上报一次）
  *    KEY_HOLD    按住电平（只读不清）
  *    KEY_DOWN    按下沿（20ms 扫描级）
  *    KEY_UP      松开沿
  *
  *  KEY0 = PA0：高电平按下（内部下拉）——单击循环页 / 长按关屏
  *  KEY1 = PB15：低电平按下（内部上拉）——单击旋转方块 / 长按重校准+重开
  ******************************************************************************
  */
#ifndef KEY_H
#define KEY_H

#include <stdint.h>

typedef enum {
    KEY0 = 0,
    KEY1,
    KEY_COUNT
} Key_Num;

/* 事件标志位（与参考工程位定义一致） */
#define KEY_SINGLE   0x01
#define KEY_DOUBLE   0x02
#define KEY_LONG     0x04
#define KEY_REPEAT   0x08
#define KEY_HOLD     0x10
#define KEY_DOWN     0x20
#define KEY_UP       0x40

/* 时间常数（参考工程默认值） */
#define KEY_SCAN_MS         20U    // 扫描周期
#define KEY_TIME_LONG_MS    1000U  // 长按阈值
#define KEY_TIME_DOUBLE_MS  100U   // 双击窗口（释放后等二次按下）
#define KEY_TIME_REPEAT_MS  50U    // 长按连发间隔

void     Key_Init(void);
void     Key_Tick(void);                    // 主循环每次调用（内部按 20ms 节流扫描）
uint8_t  Key_Check(Key_Num keyNum, uint8_t Flag);  // 1=事件在（除 HOLD 外取走即清）
uint8_t  Key_ReadLevel(Key_Num keyNum);     // 1=按下（已按极性折算）

#endif /* KEY_H */
