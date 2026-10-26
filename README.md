# stm32f103-mpu6050-oled —— MPU6050 姿态可视化装置

STM32F103C8T6（Blue Pill）通过 **软件 I2C 读取 MPU6050 6 轴数据**，互补滤波融合成欧拉角，
在 0.96 寸 SSD1306 OLED 上实时显示，**PA0 按键**切换显示状态；
外挂 **MAX7219 8x8 点阵**跑体感俄罗斯方块（倾斜移动 / 敲击旋转），OLED 作副屏。

- 库/工具链：HAL + arm-none-eabi-gcc + MSYS2 make（沿用 adc-oled 骨架）
- MPU6050：PB10/PB11 软件 I2C（400kHz，独立总线）
- OLED：PB8/PB9 软件 I2C（100kHz，沿用 adc-oled MyI2C/My_OLED）
- MAX7219：SPI1 硬件外设（PA5/PA7，PA4 软件片选）
- 通信：双 I2C 总线独立 + SPI 独立，互不干扰
- 姿态解算：互补滤波（α=0.98，不用 DMP）— 详见 [ADR-003](docs/ADR/003-no-dmp.md)

## 硬件接线

| 信号 | STM32 引脚 | 说明 |
|---|---|---|
| MPU6050 VCC | 3V3 | GY-521 模块板载稳压可接 5V，本工程按 3V3 |
| MPU6050 GND | GND | — |
| MPU6050 SCL | **PB10** | 软件 I2C 时钟（开漏，靠模块板载上拉） |
| MPU6050 SDA | **PB11** | 软件 I2C 数据 |
| MPU6050 AD0 | 悬空 | 双地址探测：先 0x68 失败回退 0x69（GY-521 浮空常被上拉到 0x69） |
| OLED VCC | 3V3 | — |
| OLED GND | GND | — |
| OLED SCL | **PB8** | 软件 I2C 时钟 |
| OLED SDA | **PB9** | 软件 I2C 数据 |
| 按键一脚 | **PA0** | 按下高电平触发（内部下拉） |
| 按键另一脚 | 3V3 | — |
| MAX7219 VCC | **5V**（datasheet 下限 4.0V；**接 3V3 不工作**——2026-10-26 真机踩坑，点阵全黑） | — |
| MAX7219 GND | GND | — |
| MAX7219 DIN | **PA7** | SPI1_MOSI。**必须接标 DIN 的那一排针**（另一排 DOUT 是级联输出，接上必然全黑——2026-10-26 真机踩坑） |
| MAX7219 CLK | **PA5** | SPI1_SCK（9MHz，CPOL=0/CPHA=1） |
| MAX7219 CS | **PA4** | 软件片选，低有效 |
| ST-Link | SWDIO/SWCLK/3V3/GND | 烧录调试 |

> 引脚无冲突；若换引脚：MPU6050 改 `User/mpu6050/mpu6050_i2c.c` 顶部宏，OLED 改 `User/MyI2C.c` 顶部宏。
>
> ⚠️ **MAX7219 模块选排针**：不少 8x8 模块出厂只焊了 **DOUT 输出排**（`VCC GND DOUT CS CLK`），输入排在另一侧只有空焊孔（`VCC GND DIN CS CLK`）。接在 DOUT 排上必然全黑（2026-10-26 真机踩坑两次：先误判 3V3 供电，后确认接线在输出排）。数据必须走 DIN 排，没焊针就飞线/补焊。

## 构建与烧录

```bash
# 在 Git Bash 或 MSYS2 下
make clean && make       # 构建，预期零警告；产物 build/mpu6050-oled.{elf,hex,bin}
make flash               # OpenOCD + ST-Link 烧录（需已连接板子）

# 真机验证（verify.py 全链路）
python D:\claude\embedded-toolkit\scripts\verify.py run

# host 单测（Git Bash，需 MSYS2 mingw64 gcc 在 PATH）
export PATH="/c/msys64/mingw64/bin:$PATH"
gcc -std=c99 -Wall -Wextra -IUser -o build/test_brt_band.exe tests/test_brt_band.c User/brt_band.c && ./build/test_brt_band.exe
gcc -std=c99 -Wall -Wextra -IUser -IUser/tetris -o build/test_tetris.exe tests/test_tetris.c User/tetris/tetris.c -lm && ./build/test_tetris.exe
```

预期构建大小：text ~35KB，bss ~3.6KB（远小于 64KB flash 上限）。

## 显示状态

| 状态 | 触发 | 显示内容 |
|---|---|---|
| 0（启动默认）| 短按回到 | 六轴原始数据（ax/ay/az + gx/gy/gz）|
| 1 | 短按 1 次 | 欧拉角（Roll/Pitch 数值，Yaw: live 占位）|
| 2 | 短按 2 次 | 2D 姿态点图（中心十字 + 参考圆 + 实时点）|
| 3 | 短按 3 次 | 俄罗斯方块副屏（NEXT 预览 + Score/Lines，棋盘在 MAX7219 点阵上）|
| 关屏 | 长按任意非关屏状态 | SSD1306 display off（采样/滤波/游戏后台继续）|

### 状态 0：六轴原始数据

```
┌──────────────────────────────┐
│  ax:    +0.02 g              │
│  ay:    -0.01 g              │
│  az:    +1.00 g              │
│                              │
│  gx:    +0.3 °/s             │
│  gy:    -0.5 °/s             │
│  gz:    +0.1 °/s             │
└──────────────────────────────┘
```

### 状态 1：欧拉角

```
┌──────────────────────────────┐
│                              │
│     Roll:   +12.3°           │
│     Pitch:  -5.7°            │
│     Yaw:    live             │  ← 文字占位（Yaw 漂移不显示数值）
│                              │
└──────────────────────────────┘
```

### 状态 2：2D 姿态点图

```
┌──────────────────────────────┐
│           ·                  │  ← 圆心 (64, 32)
│         ─┼─                  │  ← 十字坐标
│           ·                  │  ← 参考圆（半径 24 像素 ≈ 67° 警告边界）
│                              │
│         X                    │  ← 实时点（3x3 十字标记）
│                              │
└──────────────────────────────┘
```

板子平放：点位于中心
板子右倾 30°：点向右移 ~11 像素
板子前倾 30°：点向上移 ~8 像素
倾斜超 ±90°：点消失（超量程）

## 俄罗斯方块（MAX7219 点阵）

8x8 棋盘显示在 MAX7219 点阵上，OLED 状态 3 作副屏（下一块预览 + 分数/行数）。

| 操作 | 体感 | 阈值（`User/tetris/tetris.h`） |
|---|---|---|
| 左移 / 右移 | 板子前后倾（pitch 轴） | ±25° 边沿触发，回中 ±17° 再武装（防一格倾斜连移到底）；方向反了改 `TETRIS_TILT_INVERT=1` |
| 旋转 | 敲击板子 | \|a\| ≥ 1.8g 触发，回落 < 1.2g 再武装 + 200ms 冷却（防连转） |
| 重开 | 堆到顶后自动重开 | 点阵全亮（整屏红）保持 1.5s → 自动新一局 |

- 重力：500ms/格起步，每消 5 行快 100ms，下限 150ms
- 计分：1/2/3/4 行 = 1/3/5/8 分（>4 行按 4 行计，8x8 棋盘一次可消超过 4 行）
- 帧格式：`Tetris_GetFrame()` 8 字节，每字节一行，bit7 = 最左列；MAX7219 DIG0..DIG7 直写
- 点阵显示方向若不对（镜像/倒置），改 `Matrix7219_ShowFrame` 里的字节/位映射即可，逻辑层不用动
- host 单测：`tests/test_tetris.c`（19 例，覆盖碰撞/消行/计分/体感判定/重开）

## 使用注意

- **上电后保持板子静止约 1 秒**：启动阶段陀螺仪做零点偏置校准，期间按键不响应属正常；校准完成显示启动画面后即可操作。
- **Yaw 角会缓慢漂移**（无磁力计，纯陀螺仪积分），UI 显示 "Yaw: live" 文字占位，不显示数值——这是预期行为，详见 [ADR-003](docs/ADR/003-no-dmp.md)。

## 故障排查

- **黑屏排查顺序**：供电 → OLED I2C 地址 0x78（7-bit 0x3C） → SCL/SDA 接线 → 总线上拉电阻（裸屏无板载上拉时需外接 4.7kΩ×2）
- **MPU6050 无数据**：检查 SCL/SDA 接线 → 模块上拉电阻 → OLED 占用总线（应独立）
- **OLED 显示 MPU6050 FAIL**：双地址探测都失败，检查 GY-521 模块供电和 AD0 引脚
- **按键无反应**：万用表量 PA0 电压，未按下 ≈ 0V，按下 ≈ 3.3V；如相反需改 ADR-002 决策
- **Yaw 漂移是预期行为**：无磁力计无法绝对参考，UI 显示 "Yaw: live" 文字占位 — 详见 [ADR-003](docs/ADR/003-no-dmp.md)
- **MAX7219 全黑**：①**先查供电——VCC 必须 5V，3V3 低于规格下限 4.0V 完全不工作** → ②复位时看开机自检：点阵全亮 0.5s 说明模块活着 → ③再查 DIN/CLK/CS 与模块方向（DIN 是输入、DOUT 是级联输出，别接反）；SPI 速率问题可把 `matrix7219.c` 预分频改 `SPI_BAUDRATEPRESCALER_32`
- **点阵镜像/倒置**：模块翻转就能解决（物理层面），或在 `Matrix7219_ShowFrame` 里逐字节位反转 / DIG 行序反转
- **敲击不响应**：RTT 里看 `[STATE=...] ax/ay/az` 敲击瞬间的幅值，若峰值不到 1.8g，下调 `TETRIS_TAP_PEAK_G`；静置误触发则上调
- **倾斜方向反了**：`tetris.c` 的 `Tetris_Tick` 里左右两个分支对调

## 文档

- [FSD.md](docs/FSD.md) - 功能规格（10 需求点）
- [PLAN.md](docs/PLAN.md) - 实施计划（精简版）
- [ADR-001](docs/ADR/001-mpu6050-i2c-bus.md) - MPU6050 独立 I2C 总线
- [ADR-002](docs/ADR/002-key-active-high.md) - PA0 高电平触发
- [ADR-003](docs/ADR/003-no-dmp.md) - 不用 DMP，纯原始数据+互补滤波

## 复用资源

- `MyI2C.c/h` / `My_OLED.c/h` / `Drivers/` / `ThirdParty/RTT/` 全部从 `stm32f103-adc-oled/` 复制
- GCC Makefile 模板结构沿用
- OpenOCD 配置沿用
