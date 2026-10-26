#ifndef __MATRIX7219_H
#define __MATRIX7219_H

#include <stdint.h>

/* MAX7219 单块 8x8 点阵：SPI1 硬件外设（PA5=SCK, PA7=MOSI, PA4=CS 软件片控）
 * 帧格式：8 字节，每字节一行（DIG0..DIG7），bit7 = 该行最左列 */

/* 亮度档位（0x0~0xF，写 0x0A 寄存器） */
#define MATRIX_INTENSITY_MID   0x04

/**
 * @brief  初始化：GPIO/SPI1 外设 + MAX7219 寄存器配置（decode off/scan 7/intensity/shutdown off）
 */
void Matrix7219_Init(void);

/**
 * @brief  写一个 MAX7219 寄存器（CS 拉低 → 16bit 移位 → CS 上升锁存）
 */
void Matrix7219_WriteRegister(uint8_t reg, uint8_t val);

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
