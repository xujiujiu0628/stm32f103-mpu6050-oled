#ifndef __TETRIS_H
#define __TETRIS_H

#include <stdint.h>

/* 棋盘 8x8；帧 = 8 字节，每字节一行，bit7 = 最左列（与 MAX7219 DIG0..DIG7 行序一致） */

/* 倾斜控制阈值（度）：超出触发一次侧移，回中 ±TETRIS_TILT_DEADBAND 内再武装 */
#define TETRIS_TILT_THRESHOLD    25.0f
#define TETRIS_TILT_DEADBAND     8.0f

/* 敲击检测：|a| 超过尖峰阈值触发旋转，200ms 冷却；回落 < 回落阈值再武装 */
#define TETRIS_TAP_PEAK_G        1.8f
#define TETRIS_TAP_RELEASE_G     1.2f
#define TETRIS_TAP_COOLDOWN_MS   200U

/* 重力下落周期（ms/格），每次消行加速 */
#define TETRIS_DROP_BASE_MS      800U

/**
 * @brief  重置游戏：清棋盘、清分、出第一块
 */
void Tetris_Reset(void);

/**
 * @brief  游戏主 tick（10ms 节拍，主循环调用）
 * @param  roll  当前横滚角（度）
 * @param  ax/ay/az  加速度（g）
 */
void Tetris_Tick(float roll, float ax, float ay, float az);

/**
 * @brief  手动侧移（左/右），成功 1 / 撞墙或撞块 0
 */
uint8_t Tetris_ShiftLeft(void);
uint8_t Tetris_ShiftRight(void);

/**
 * @brief  手动旋转（敲击处理也走这里）
 */
void Tetris_Rotate(void);

/**
 * @brief  渲染帧：棋盘 + 活动块合并后 8 字节（bit7=最左列）
 */
const uint8_t *Tetris_GetFrame(void);

/**
 * @brief  状态查询（OLED 副屏用）
 */
uint16_t Tetris_GetScore(void);
uint16_t Tetris_GetLines(void);
const uint8_t *Tetris_GetNextShape(void);   /* 下一块 8 字节点阵（居中） */
uint8_t Tetris_IsGameOver(void);

/* 测试钩子：直接覆写棋盘（绕开随机出块，锚定消行/计分路径） */
void Tetris_DebugSetBoard(const uint8_t rows[8]);

#endif
