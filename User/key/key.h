/**
  ******************************************************************************
  * @file    key.h
  * @brief   双键非阻塞输入模块（适度移植自桌面工程 01-全功能按键，见
  *          docs/FSD.md §6 与本次改造注记）
  *
  *  架构：每键独立状态机（v2 时间窗消抖，2026-09-06 真机验证的版本），
  *  键位配置表驱动。不移植 01 工程的双击/连发（本项目用不到——YAGNI）。
  *
  *  KEY0 = PA0：高电平按下（内部下拉）——短按循环页 / 长按关屏，语义不变
  *  KEY1 = PB15：低电平按下（内部上拉）——仅长按有意义（陀螺仪重校准），
  *               短按事件同样上报，由消费方决定忽略
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

typedef enum {
    KEY_EVENT_NONE  = 0,
    KEY_EVENT_SHORT = 1,    // 短按：按下持续 < KEY_LONG_MS（释放时上报）
    KEY_EVENT_LONG  = 2,    // 长按：按下满 1000ms 立即上报一次
} KeyEvent_t;

#define KEY_DEBOUNCE_MS   50U    // 电平稳定窗口（消抖）
#define KEY_LONG_MS       1000U  // 长按阈值

void     Key_Init(void);
void     Key_Tick(void);                    // 每主循环调用（毫秒级足够）
KeyEvent_t Key_GetEvent(Key_Num keyNum);    // 取走即清（每键独立信箱）
uint8_t  Key_ReadLevel(Key_Num keyNum);     // 1=按下（已按极性折算）

#endif /* KEY_H */
