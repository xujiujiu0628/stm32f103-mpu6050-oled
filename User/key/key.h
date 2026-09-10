#ifndef __KEY_H
#define __KEY_H

#include <stdint.h>

/* 按键事件 */
typedef enum {
    KEY_EVENT_NONE  = 0,
    KEY_EVENT_SHORT = 1,    // 短按：按下持续 < KEY_LONG_MS
    KEY_EVENT_LONG  = 2,    // 长按：按下持续 ≥ KEY_LONG_MS
} KeyEvent_t;

/* 时序参数 */
#define KEY_DEBOUNCE_MS   50U    // 消抖窗口
#define KEY_LONG_MS       1000U  // 长按阈值

/**
 * @brief  初始化：PA0 内部下拉输入
 * @note   按键按下 → 高电平；不按下 → 低电平（内部下拉保证）
 */
void Key_Init(void);

/**
 * @brief  周期性 tick（建议 1ms 周期，在 SysTick 或 10ms tick 中调用）
 * @note   非阻塞：内部状态机维护去抖和事件识别
 */
void Key_Tick(void);

/**
 * @brief  获取最近一次按键事件（消费式：调用后清空内部事件）
 */
KeyEvent_t Key_GetEvent(void);

/**
 * @brief  按键当前电平（调试用）
 */
uint8_t Key_ReadLevel(void);

#endif
