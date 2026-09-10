#ifndef __MPU6050_H
#define __MPU6050_H

#include <stdint.h>

/* MPU6050 寄存器表（精简到工程必需项） */
#define MPU6050_REG_SMPLRT_DIV    0x19    // 采样分频（默认 0 → 1kHz）
#define MPU6050_REG_CONFIG        0x1A    // DLPF 配置（默认 0）
#define MPU6050_REG_GYRO_CONFIG   0x1B    // 陀螺仪量程（默认 0 → ±250°/s）
#define MPU6050_REG_ACCEL_CONFIG  0x1C    // 加速度量程（默认 0 → ±2g）
#define MPU6050_REG_INT_PIN_CFG   0x37    // 中断引脚配置
#define MPU6050_REG_INT_ENABLE    0x38    // 中断使能
#define MPU6050_REG_ACCEL_XOUT_H  0x3B    // 加速度 X 高字节
#define MPU6050_REG_TEMP_OUT_H    0x41    // 温度高字节
#define MPU6050_REG_GYRO_XOUT_H   0x43    // 陀螺仪 X 高字节
#define MPU6050_REG_PWR_MGMT_1    0x6B    // 电源管理 1
#define MPU6050_REG_PWR_MGMT_2    0x6C    // 电源管理 2
#define MPU6050_REG_WHO_AM_I      0x75    // 身份寄存器（期望 0x68）

#define MPU6050_WHO_AM_I_VAL      0x68    // WHO_AM_I 期望返回值（0x68/0x69 同源芯片）
#define MPU6050_ADDR_AD0_LOW      0x68    // AD0=0 → 0x68
#define MPU6050_ADDR_AD0_HIGH     0x69    // AD0=1 → 0x69

/* 加速度量程 ±2g → 16384 LSB/g；陀螺仪量程 ±250°/s → 131 LSB/(°/s) */
#define MPU6050_ACCEL_SENSITIVITY  16384.0f
#define MPU6050_GYRO_SENSITIVITY   131.0f
#define MPU6050_TEMP_OFFSET        0       // 温度公式参见 datasheet：T = T_OUT/340 + 36.53

/* 互补滤波参数：α=0.98 → 陀螺仪权重 98%，加速度计 2%（行业标准值） */
#define MPU6050_FILTER_ALPHA       0.98f

/* 采样周期：100Hz → dt=10ms（用 SysTick 实测） */
#define MPU6050_SAMPLE_PERIOD_MS   10

/* 6 轴原始数据（g 或 °/s） */
typedef struct {
    float ax, ay, az;     // 加速度（g）
    float gx, gy, gz;     // 陀螺仪（°/s）
    float temperature;    // 温度（°C，工程未用，保留供调试）
} MPU6050_RawData_t;

/* 欧拉角（互补滤波输出） */
typedef struct {
    float roll;           // 横滚角（°，绕 X 轴，-180~+180）
    float pitch;          // 俯仰角（°，绕 Y 轴，-90~+90）
    float yaw;            // 偏航角（°，绕 Z 轴，无磁力计会缓慢漂移）
    float yaw_drift_rate; // yaw 漂移率（°/s，调试用，UI 不显示）
} MPU6050_Euler_t;

/* 模块状态（启动期探测用） */
typedef enum {
    MPU6050_STATE_UNINIT = 0,
    MPU6050_STATE_OK,
    MPU6050_STATE_FAIL
} MPU6050_State_t;

/**
 * @brief  初始化：探测地址 + 配置寄存器 + 复位欧拉角
 * @retval 0=成功（含探测到 0x68 或 0x69），1=失败（无应答或 WHO_AM_I 不匹配）
 */
uint8_t MPU6050_Init(void);

/**
 * @brief  读取 6 轴原始数据（调用前需 I2C 总线空闲；本函数为阻塞读 ~1ms @ 400kHz）
 * @param  out  输出数据结构
 * @retval 0=成功，1=失败
 */
uint8_t MPU6050_ReadRaw(MPU6050_RawData_t *out);

/**
 * @brief  互补滤波 tick：融合加速度+陀螺仪算欧拉角
 * @note   应在固定周期调用（100Hz），dt 由内部 SysTick 测得
 * @param  euler  输出欧拉角
 * @retval 0=成功
 */
uint8_t MPU6050_UpdateEuler(MPU6050_Euler_t *euler);

/**
 * @brief  陀螺仪零点偏置校准（静止采样求平均）
 * @note   上电后板子须保持静止 ~1s；校准后 gx/gy/gz 读值自动减偏置；
 *         不做校准则静止时陀螺仪读数非 0，互补滤波会持续积分偏置导致姿态漂移
 * @retval 0=成功
 */
uint8_t MPU6050_CalibrateGyroBias(void);

/**
 * @brief  获取最近一次欧拉角（用于显示）
 */
const MPU6050_Euler_t *MPU6050_GetEuler(void);

/**
 * @brief  获取最近一次原始数据（用于显示/调试）
 */
const MPU6050_RawData_t *MPU6050_GetRaw(void);

/**
 * @brief  模块状态（启动期诊断用）
 */
MPU6050_State_t MPU6050_GetState(void);

/**
 * @brief  探测到的实际 I2C 地址（0x68 或 0x69）
 */
uint8_t MPU6050_GetAddress(void);

#endif
