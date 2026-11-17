#ifndef __MATRIX7219_H
#define __MATRIX7219_H

#include <stdint.h>

/* MAX7219 单块 8x8 点阵：软件位翻转（PA6=CLK, PA7=DIN, PA4=CS 软件片控）
 * 帧格式：8 字节，每字节一行（DIG0..DIG7），bit7 = 该行最左列 */

/* 亮度档位（0x0~0xF，写 0x0A 寄存器）
 * 2026-11-17 排查: 原值 0x04（27%）在软位翻转下几乎看不见，先临时拉满做对照 */
#define MATRIX_INTENSITY_MID   0x0F

/**
 * @brief  初始化：GPIO/SPI1 外设 + MAX7219 寄存器配置（decode off/scan 7/intensity/shutdown off）
 * @retval 0=所有寄存器写成功；非 0=首个失败的 HAL 状态码（SPI 超时/忙）
 */
uint8_t Matrix7219_Init(void);

/**
 * @brief  电源稳定后重新下发配置（decode/scan/intensity/shutdown）+ 清屏。
 *         2026-11-17 真机: 上电瞬间的配置可能被模块丢掉 → 停在 shutdown
 *         （display test 照常亮、数字寄存器不显示）。幂等，可重复调用。
 */
void Matrix7219_Reconfig(void);

/**
 * @brief  写一个 MAX7219 寄存器（CS 拉低 → 16bit 移位 → CS 上升锁存）
 * @retval 恒 0（软件位翻转没有 HAL 状态码可报；CS 时序在函数内完成）
 */
uint8_t Matrix7219_WriteRegister(uint8_t reg, uint8_t val);

/**
 * @brief  显示测试：on=1 全点亮 / on=0 恢复正常显示（硬件自检用）
 */
void Matrix7219_DisplayTest(uint8_t on);

/**
 * @brief  刷整帧 8 字节（8 次寄存器写，DIG0..DIG7 = frame[0..7]）
 */
void Matrix7219_ShowFrame(const uint8_t frame[8]);

/**
 * @brief  亮度 0x0~0xF（透传 0x0A 寄存器）
 */
void Matrix7219_SetIntensity(uint8_t intensity);

/**
 * @brief  关闭/点亮点阵（shutdown 寄存器）
 */
void Matrix7219_Shutdown(uint8_t off);

#endif
