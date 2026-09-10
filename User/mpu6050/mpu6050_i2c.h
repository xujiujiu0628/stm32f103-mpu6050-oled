#ifndef __MPU6050_I2C_H
#define __MPU6050_I2C_H

#include "stm32f1xx_hal.h"

/* MPU6050 独立 I2C 总线（PB10=SCL, PB11=SDA），与 OLED 共享时序实现但不复用 OLED 的 PB8/PB9 */
void MPU6050_I2C_Init(void);
void MPU6050_I2C_Start(void);
void MPU6050_I2C_Stop(void);
void MPU6050_I2C_SendByte(uint8_t Byte);
uint8_t MPU6050_I2C_ReceiveByte(void);
void MPU6050_I2C_SendAck(uint8_t AckBit);
uint8_t MPU6050_I2C_ReceiveAck(void);

/* 错误码（与 MPU6050 驱动共享返回值） */
#define MPU6050_I2C_OK     0
#define MPU6050_I2C_ERR    1

#endif
