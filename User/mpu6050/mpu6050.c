/**
  ******************************************************************************
  * @file    mpu6050.c
  * @brief   MPU6050 驱动：地址探测 + 6 轴读取 + 互补滤波
  *
  * @架构    软件 I2C + 阻塞读 + 100Hz 采样 → 主循环调用 UpdateEuler
  * @依赖    mpu6050_i2c.c（软件 I2C 总线）、stm32f1xx_hal.h（HAL 库）
  * @注意    dt 来自 SysTick 实测；互补滤波的 α=0.98 是工程标准值
  ******************************************************************************
  */
#include "mpu6050.h"
#include "mpu6050_i2c.h"
#include "stm32f1xx_hal.h"
#include <math.h>

/* 外部 SysTick 计时（main.c 提供） */
extern volatile uint32_t g_millis;

/* 模块内部状态 */
static MPU6050_State_t  s_state    = MPU6050_STATE_UNINIT;
static uint8_t          s_addr     = 0;          // 探测到的 I2C 地址
static MPU6050_RawData_t s_raw      = {0};        // 最新原始数据
static MPU6050_Euler_t   s_euler    = {0,0,0,0};  // 最新欧拉角
static uint32_t         s_last_tick = 0;          // 上次滤波时间戳

/* 陀螺仪零点偏置（°/s，启动校准时测得；ReadRaw 后自动扣除） */
static float s_bias_gx = 0.0f;
static float s_bias_gy = 0.0f;
static float s_bias_gz = 0.0f;

/* ---------------- 内部 I2C 读写辅助 ---------------- */

static uint8_t mpu6050_write_reg(uint8_t reg, uint8_t val)
{
    MPU6050_I2C_Start();
    MPU6050_I2C_SendByte(s_addr << 1);          // 写地址
    if (MPU6050_I2C_ReceiveAck() != 0) { MPU6050_I2C_Stop(); return 1; }
    MPU6050_I2C_SendByte(reg);
    if (MPU6050_I2C_ReceiveAck() != 0) { MPU6050_I2C_Stop(); return 1; }
    MPU6050_I2C_SendByte(val);
    if (MPU6050_I2C_ReceiveAck() != 0) { MPU6050_I2C_Stop(); return 1; }
    MPU6050_I2C_Stop();
    return 0;
}

static uint8_t mpu6050_read_reg(uint8_t reg, uint8_t *val)
{
    // 第一步：写寄存器地址
    MPU6050_I2C_Start();
    MPU6050_I2C_SendByte(s_addr << 1);
    if (MPU6050_I2C_ReceiveAck() != 0) { MPU6050_I2C_Stop(); return 1; }
    MPU6050_I2C_SendByte(reg);
    if (MPU6050_I2C_ReceiveAck() != 0) { MPU6050_I2C_Stop(); return 1; }
    // 第二步：重启 + 读
    MPU6050_I2C_Start();
    MPU6050_I2C_SendByte((s_addr << 1) | 1);
    if (MPU6050_I2C_ReceiveAck() != 0) { MPU6050_I2C_Stop(); return 1; }
    *val = MPU6050_I2C_ReceiveByte();
    MPU6050_I2C_SendAck(1);                     // NACK 终止单字节读
    MPU6050_I2C_Stop();
    return 0;
}

static uint8_t mpu6050_read_burst(uint8_t start_reg, uint8_t *buf, uint8_t len)
{
    // 写起始寄存器
    MPU6050_I2C_Start();
    MPU6050_I2C_SendByte(s_addr << 1);
    if (MPU6050_I2C_ReceiveAck() != 0) { MPU6050_I2C_Stop(); return 1; }
    MPU6050_I2C_SendByte(start_reg);
    if (MPU6050_I2C_ReceiveAck() != 0) { MPU6050_I2C_Stop(); return 1; }
    // 重启 + burst 读
    MPU6050_I2C_Start();
    MPU6050_I2C_SendByte((s_addr << 1) | 1);
    if (MPU6050_I2C_ReceiveAck() != 0) { MPU6050_I2C_Stop(); return 1; }
    for (uint8_t i = 0; i < len; i++)
    {
        buf[i] = MPU6050_I2C_ReceiveByte();
        MPU6050_I2C_SendAck(i == (len - 1) ? 1 : 0);  // 最后一字节 NACK，其余 ACK
    }
    MPU6050_I2C_Stop();
    return 0;
}

/* ---------------- 地址探测 ---------------- */

/**
 * @brief  尝试在 addr 上读 WHO_AM_I，匹配 0x68 视为成功
 */
static uint8_t try_probe(uint8_t addr)
{
    uint8_t who = 0;
    s_addr = addr;
    if (mpu6050_read_reg(MPU6050_REG_WHO_AM_I, &who) != 0) return 1;
    return (who == MPU6050_WHO_AM_I_VAL) ? 0 : 1;
}

/* ---------------- 公开 API ---------------- */

uint8_t MPU6050_Init(void)
{
    MPU6050_I2C_Init();
    HAL_Delay(100);    // 上电稳定

    // 双地址探测：先 0x68（AD0=0 期望），失败回退 0x69（AD0 浮空被上拉）
    if (try_probe(MPU6050_ADDR_AD0_LOW) == 0)
    {
        s_state = MPU6050_STATE_OK;
    }
    else if (try_probe(MPU6050_ADDR_AD0_HIGH) == 0)
    {
        s_state = MPU6050_STATE_OK;
    }
    else
    {
        s_state = MPU6050_STATE_FAIL;
        s_addr  = 0;
        return 1;
    }

    // 配置
    mpu6050_write_reg(MPU6050_REG_PWR_MGMT_1,  0x00);  // 唤醒（清 SLEEP 位）
    HAL_Delay(10);
    mpu6050_write_reg(MPU6050_REG_SMPLRT_DIV,  0x09);  // 1kHz / (1+9) = 100Hz
    mpu6050_write_reg(MPU6050_REG_CONFIG,      0x03);  // DLPF=3 → 44Hz 带宽
    mpu6050_write_reg(MPU6050_REG_GYRO_CONFIG, 0x00);  // ±250°/s
    mpu6050_write_reg(MPU6050_REG_ACCEL_CONFIG,0x00);  // ±2g
    mpu6050_write_reg(MPU6050_REG_PWR_MGMT_2,  0x00);  // 不进入低功耗

    /* 陀螺仪零点偏置校准（板子须保持静止 ~1s）；
       不做校准则 gx 恒为 -5.5°/s 之类的出厂偏置，互补滤波会持续积分偏置 → 姿态漂移 */
    MPU6050_CalibrateGyroBias();

    // 初始欧拉角从加速度计算（陀螺仪积分尚未建立）
    s_last_tick = g_millis;
    s_euler.roll  = 0; s_euler.pitch = 0; s_euler.yaw = 0;
    s_euler.yaw_drift_rate = 0;
    return 0;
}

uint8_t MPU6050_ReadRaw(MPU6050_RawData_t *out)
{
    if (s_state != MPU6050_STATE_OK) return 1;

    uint8_t buf[14];
    if (mpu6050_read_burst(MPU6050_REG_ACCEL_XOUT_H, buf, 14) != 0) return 1;

    // 数据格式：axH axL ayH ayL azH azL tempH tempL gxH gxL gyH gyL gzH gzL
    int16_t ax_raw = (int16_t)((buf[0]  << 8) | buf[1]);
    int16_t ay_raw = (int16_t)((buf[2]  << 8) | buf[3]);
    int16_t az_raw = (int16_t)((buf[4]  << 8) | buf[5]);
    int16_t t_raw  = (int16_t)((buf[6]  << 8) | buf[7]);
    int16_t gx_raw = (int16_t)((buf[8]  << 8) | buf[9]);
    int16_t gy_raw = (int16_t)((buf[10] << 8) | buf[11]);
    int16_t gz_raw = (int16_t)((buf[12] << 8) | buf[13]);

    s_raw.ax = (float)ax_raw / MPU6050_ACCEL_SENSITIVITY;
    s_raw.ay = (float)ay_raw / MPU6050_ACCEL_SENSITIVITY;
    s_raw.az = (float)az_raw / MPU6050_ACCEL_SENSITIVITY;
    s_raw.temperature = (float)t_raw / 340.0f + 36.53f;
    /* 陀螺仪读值扣除零点偏置（未校准时 bias=0，行为与原版一致） */
    s_raw.gx = (float)gx_raw / MPU6050_GYRO_SENSITIVITY - s_bias_gx;
    s_raw.gy = (float)gy_raw / MPU6050_GYRO_SENSITIVITY - s_bias_gy;
    s_raw.gz = (float)gz_raw / MPU6050_GYRO_SENSITIVITY - s_bias_gz;

    if (out) *out = s_raw;
    return 0;
}

uint8_t MPU6050_CalibrateGyroBias(void)
{
    if (s_state != MPU6050_STATE_OK) return 1;

    /* 校准前清零偏置，确保读的是原始值 */
    s_bias_gx = 0.0f;
    s_bias_gy = 0.0f;
    s_bias_gz = 0.0f;
    s_last_tick = g_millis;

    /* 静止采样 100 次求平均（板子须保持静止，约 1s） */
    enum { N = 100 };
    float sx = 0.0f, sy = 0.0f, sz = 0.0f;
    MPU6050_RawData_t r;
    for (int i = 0; i < N; i++)
    {
        if (MPU6050_ReadRaw(&r) == 0)
        {
            sx += r.gx;
            sy += r.gy;
            sz += r.gz;
            /* 10ms 间隔采样，1s 覆盖 */
            uint32_t t0 = g_millis;
            while ((g_millis - t0) < 10) { }
        }
    }
    s_bias_gx = sx / N;
    s_bias_gy = sy / N;
    s_bias_gz = sz / N;
    return 0;
}

uint8_t MPU6050_UpdateEuler(MPU6050_Euler_t *euler)
{
    if (s_state != MPU6050_STATE_OK) return 1;
    uint8_t r = MPU6050_ReadRaw(NULL);
    if (r != 0) return 1;

    // 计算 dt（秒）
    uint32_t now = g_millis;
    float dt = (now - s_last_tick) / 1000.0f;
    if (dt <= 0.0f || dt > 0.5f) dt = 0.01f;   // 异常回退 10ms（首次调用 / 长时间停转）
    s_last_tick = now;

    // 加速度计算倾角（roll/pitch）
    float acc_roll  = atan2f(s_raw.ay, s_raw.az) * 57.29578f;          // rad → deg
    float acc_pitch = atan2f(-s_raw.ax, sqrtf(s_raw.ay * s_raw.ay + s_raw.az * s_raw.az)) * 57.29578f;

    // 互补滤波：陀螺仪短期信任 + 加速度长期纠正
    s_euler.roll  = MPU6050_FILTER_ALPHA * (s_euler.roll  + s_raw.gx * dt) + (1.0f - MPU6050_FILTER_ALPHA) * acc_roll;
    s_euler.pitch = MPU6050_FILTER_ALPHA * (s_euler.pitch + s_raw.gy * dt) + (1.0f - MPU6050_FILTER_ALPHA) * acc_pitch;
    s_euler.yaw  += s_raw.gz * dt;        // yaw 纯陀螺仪积分（接受漂移）

    // yaw 漂移率（调试用，UI 不显示数值）
    static float prev_yaw = 0;
    s_euler.yaw_drift_rate = (s_euler.yaw - prev_yaw) / dt;
    prev_yaw = s_euler.yaw;

    if (euler) *euler = s_euler;
    return 0;
}

const MPU6050_Euler_t *MPU6050_GetEuler(void)    { return &s_euler; }
const MPU6050_RawData_t *MPU6050_GetRaw(void)    { return &s_raw; }
MPU6050_State_t  MPU6050_GetState(void)          { return s_state; }
uint8_t          MPU6050_GetAddress(void)        { return s_addr; }
