#ifndef __TETRIS_H
#define __TETRIS_H

#include <stdint.h>

/* 棋盘 8x8；帧 = 8 字节，每字节一行，bit7 = 最左列（与 MAX7219 DIG0..DIG7 行序一致） */

/* 倾斜控制阈值（度）：超出触发一次侧移，回中 ±TETRIS_TILT_DEADBAND 内再武装
 * @控制轴  用 pitch（前后倾）——手持板子自然倾斜产生的就是 pitch；roll 是绕长轴滚转
 * @方向    TETRIS_TILT_INVERT=1 时左右映射对调（现场手感方向反了就改这个） */
#define TETRIS_TILT_THRESHOLD    25.0f
#define TETRIS_TILT_DEADBAND     8.0f
#define TETRIS_TILT_INVERT       0

/* 敲击检测：|a| 超过尖峰阈值触发旋转，200ms 冷却；回落 < 回落阈值再武装 */
#define TETRIS_TAP_PEAK_G        1.8f
#define TETRIS_TAP_RELEASE_G     1.2f
#define TETRIS_TAP_COOLDOWN_MS   200U

/* 重力下落周期（ms/格）：500ms 起步，每消 5 行快 100ms，下限 150ms
 * 软降：roll（左右滚转）超过 ±20° 时以 60ms/格 快速下落 */
#define TETRIS_DROP_BASE_MS      500U
#define TETRIS_DROP_MIN_MS       150U
#define TETRIS_SOFTDROP_ROLL_DEG 20.0f
#define TETRIS_DROP_SOFT_MS      60U

/* 游戏结束：点阵全亮提示，保持 TETRIS_OVER_HOLD_MS 后自动重开 */
#define TETRIS_OVER_HOLD_MS      1500U

/**
 * @brief  重置游戏：清棋盘、清分、出第一块
 */
void Tetris_Reset(void);

/**
 * @brief  上电播种：用传感器噪声/时间戳打散出块序列（非 0 才生效）
 */
void Tetris_Seed(uint32_t seed);

/**
 * @brief  游戏主 tick（10ms 节拍，主循环调用）
 * @param  tilt_deg  倾斜角（pitch，度）—— 左右移动
 * @param  roll_deg  滚转角（度）—— |roll| ≥ 20° 时软降加速
 * @param  ax/ay/az  加速度（g）
 */
void Tetris_Tick(float tilt_deg, float roll_deg, float ax, float ay, float az);

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

/* 当前/下一块的形状编号（0..6，调试/日志用） */
uint8_t Tetris_GetCurrentShapeId(void);
uint8_t Tetris_GetNextShapeId(void);

/* 测试钩子：直接覆写棋盘（绕开随机出块，锚定消行/计分路径） */
void Tetris_DebugSetBoard(const uint8_t rows[8]);

#endif
