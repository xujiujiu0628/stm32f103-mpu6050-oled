# PLAN: stm32f103-mpu6050-oled 实施计划（精简版）

## 概述

完整实施计划见 `C:\Users\34354\.claude\plans\stm32f103c8t6-cube-fsd-mpu6050-oled-oled-bubbly-ripple.md`。
本文档是项目内的精简版，列出已完成的实施步骤和验证结果。

## 模块清单

| 模块 | 路径 | 状态 |
|---|---|---|
| MPU6050 软件 I2C | User/mpu6050/mpu6050_i2c.c/h | ✅ 完成 |
| MPU6050 驱动+滤波 | User/mpu6050/mpu6050.c/h | ✅ 完成 |
| PA0 按键 | User/key/key.c/h | ✅ 完成 |
| OLED 状态机 | User/oled_view/oled_view.c/h | ✅ 完成 |
| OLED 软件 I2C | User/MyI2C.c/h | ✅ 复用 adc-oled |
| SSD1306 驱动 | User/My_OLED.c/h | ✅ 复用 adc-oled |
| 主循环 | User/main.c | ✅ 完成 |
| SysTick 中断 | User/stm32f1xx_it.c | ✅ 完成 |

## 构建结果

- text: 28580B（27.9KB，含 -u _printf_float 浮点 printf + 校准逻辑）
- data: 484B
- bss: 3332B
- 总计: 32396B（约 31.6KB）
- 警告/错误: 0/0

## 真机验证 ✅（2026-09-06 完成）

- `make flash` Verified OK
- `verify.py` verdict=**ok**，8/8 期望（5 pass + 3 合理 xfail）
- 物理测试全过：4 状态切换 / 倾斜点动跟随 / 长短按 / 关屏唤醒 / 姿态点平放居中

### 真机逼出的 3 个 bug（unit test 未覆盖，全在真机现形）

| bug | 根因 | 修复 |
|---|---|---|
| MPU6050 数据显示为空 | nano.specs 默认不链浮点 printf，`%.2f` 输出空串 | Makefile LDFLAGS 加 `-u _printf_float` |
| PA0=0 却疯狂 fire KEY LONG + MPU 间歇失败 | My_OLED.c `ShowImage` 用 `DisplayBuf[Y/8+j+1]` 处理跨页，字符在 y=56（页 7）时写页 8 越界 128B，污染 .bss 相邻 static 变量 | ShowImage/ClearArea 加边界保护；oled_view 全用 6×8 小字 |
| 姿态点平放偏移 + roll 持续漂移 | 陀螺仪出厂偏置 -5.5°/s 未被校准，互补滤波持续积分；OLED 25ms 阻塞拉低采样率稀释纠正力 | `MPU6050_CalibrateGyroBias()` 上电静止 1s 校准；OLED I2C 5→3μs 提速保采样率 |

## 复用资源

- adc-oled 工程的 Drivers/、ThirdParty/RTT/、MyI2C.c、My_OLED.c、STM32F103xx_FLASH.ld、startup_stm32f103xb.s、openocd.cfg、stm32f1xx_hal_conf.h
- GCC Makefile 模板结构
- OpenOCD 配置
