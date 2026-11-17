# stm32f103-mpu6050-oled —— MPU6050 姿态可视化装置

STM32F103C8T6（Blue Pill）通过 **软件 I2C 读取 MPU6050 6 轴数据**，互补滤波融合成欧拉角，
在 0.96 寸 SSD1306 OLED 上实时显示，**PA0 按键**切换显示状态；
外挂 **MAX7219 8x8 点阵**跑体感俄罗斯方块（倾斜移动 / 敲击旋转），OLED 作副屏。

- 库/工具链：HAL + arm-none-eabi-gcc + MSYS2 make（沿用 adc-oled 骨架）
- MPU6050：PB10/PB11 软件 I2C（400kHz，独立总线）
- OLED：PB8/PB9 软件 I2C（100kHz，沿用 adc-oled MyI2C/My_OLED）
- MAX7219：软件位翻转（CLK=PA6、DIN=PA7，PA4 软件片选）
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
| MAX7219 DIN | **PA7** | 软件位翻转 DIN。**必须接标 DIN 的那一排针**（另一排 DOUT 是级联输出，接上必然全黑——2026-10-26 真机踩坑） |
| MAX7219 CLK | **PA6** | 软件位翻转 CLK（2026-11-17 由 PA5 改，PA5 让给电位器 ADC；原 SPI1 硬件外设退役） |
| MAX7219 CS | **PA4** | 软件片选，低有效 |
| 电位器抽头 | **PA5** | ADC1_IN5（2026-11-17 由 PA6 改） |
| 电位器两端 | 3V3 / GND | — |
| ST-Link | SWDIO/SWCLK/3V3/GND | 烧录调试 |
| IR OUT | **PB12** | 0038 一体化接收头输出，EXTI 双边沿（空闲高） |
| IR VCC | **3.3V** | 接收头 2.7–5.5V 可用；用 3.3V 免电平转换 |
| IR GND | **GND** | — |

> 引脚无冲突；若换引脚：MPU6050 改 `User/mpu6050/mpu6050_i2c.c` 顶部宏，OLED 改 `User/MyI2C.c` 顶部宏。
>
> ⚠️ **MAX7219 模块选排针**：不少 8x8 模块出厂只焊了 **DOUT 输出排**（`VCC GND DOUT CS CLK`），输入排在另一侧只有空焊孔（`VCC GND DIN CS CLK`）。接在 DOUT 排上必然全黑（2026-10-26 真机踩坑两次：先误判 3V3 供电，后确认接线在输出排）。数据必须走 DIN 排，没焊针就飞线/补焊。

> 红外接收头供电建议按 datasheet 串 100Ω + 对地并 100µF；OUT 已开 MCU 内部上拉，长线（>15cm）再并 10k。
> 0038 三脚定义（OUT/GND/VCC）各厂不一致，**以实物丝印为准**；接反不坏，静态量 OUT 应为 ≈3.3V。
>
> 引脚号别混：IR 用的是 **PB12**（EXTI15_10）。

## 构建与烧录

```bash
# 在 Git Bash 或 MSYS2 下
make clean && make       # 构建，预期零警告；产物 build/mpu6050-oled.{elf,hex,bin}
make flash               # OpenOCD + ST-Link 烧录（需已连接板子）

# 真机验证（verify.py 全链路）
python D:\claude-embed\embedded-toolkit\scripts\verify.py --project .

# host 单测（Git Bash，需 MSYS2 mingw64 gcc 在 PATH）
export PATH="/c/msys64/mingw64/bin:$PATH"
gcc -std=c99 -Wall -Wextra -IUser -o build/test_brt_band.exe tests/test_brt_band.c User/brt_band.c && ./build/test_brt_band.exe
gcc -std=c99 -Wall -Wextra -IUser -IUser/tetris -o build/test_tetris.exe tests/test_tetris.c User/tetris/tetris.c -lm && ./build/test_tetris.exe
```

预期构建大小：text ~37KB，bss ~3.6KB（远小于 64KB flash 上限）。

### 真机验证要人工按键（4 条 FR，2026-10-28 实测）

`verify.py` 的 13 项里 **9 项自动可过**，下面 4 项要人在采集窗口内按键，否则必然 FAIL：

| FR | 期望文本 | 触发动作 |
|---|---|---|
| FR-CAL-01 | `[CAL] done` | **PB15 长按** ~1s（陀螺仪重校准 + 重开一局） |
| FR-KEY-01 | `[KEY] SHORT` | **PA0 短按**（每按一次打印一次） |
| FR-OLED-02 | `[STATE=3 last=` | **PA0 短按 ×3**，停在俄罗斯方块页（state 3） |
| FR-KEY-02 | `[KEY] LONG` | **PA0 长按** ~1s（= 关屏，**必须最后做**） |

推荐顺序（`--timeout 60` 起步，烧写后板子重启即开始计时）：

1. PB15 长按 ~1s
2. PA0 短按 ×3，间隔约 1s
3. PA0 长按 ~1s，**最后**

排序原因：PA0 长按会把屏关掉，先按就进不到 state 3，FR-OLED-02 永远拿不到。

**失败时怎么认**：4 条一起 FAIL、capture 里 `[DIAG] PA0=1` 出现 0 次 → 是没人按，不是固件坏了。
`verify.py` 此时给的 `agent_hint`（"检查 registry 注册顺序和 printf 格式"）是误导，别顺着它改代码。

### 红外相关人工核对（v0.11 新增，不进机器断言）

三条都是「某段日志不出现」的负向判定，与 FR-IR-02/04 的正向断言同窗冲突，故不做机器断言、改走人工核对；
已在 `.workbench/expectations.json` 的 `waived` 数组登记为豁免（FR-IR-05/06/07，evidence 指向本节）。

| 项 | 动作 | 通过判据 |
|---|---|---|
| FR-IR-05 | IR 模式下倾斜板子 | 方块不横移；重力仍在下落 |
| FR-IR-06 | 切回 MPU 模式后按遥控旋转键 | 无 `[IR] rotate`，方块不转 |
| FR-IR-07 | IR 模式下按住旋转键 2s | 只出现 1 行 `[IR] rotate` |

## 刷屏与主循环节拍（2026-10-26 真机实测）

软 I2C 刷一整帧 SSD1306 是 1024 字节，实测**单次阻塞 132ms**——主循环一个迭代只剩一次采样机会。
`User/oled_view/oled_view.c` 的 `OLEDS_SLICE_ENABLE` 决定刷屏方式（默认 1 = 切片）：

| 指标（RTT `[PERF]` 心跳，1s 一行） | 整帧刷 `=0` | 切片刷 `=1`（默认） |
|---|---|---|
| 单次刷屏阻塞 | 132.1ms | 8.4ms（64 字节/片，16 片/帧） |
| 排版+绘制（纯 RAM） | 2.2ms | 2.5ms |
| 实际 MPU 采样率 | 7.3Hz（间隔均值 137ms） | 96~97Hz（均值 10.3ms，峰值 14ms） |
| 主循环迭代 | 7 次/s（无"轻迭代"） | 102 次/s（97 次不刷屏） |
| OLED 帧率 | 7fps | 5~6fps |

- 帧率略降是切片的代价（每片多一次定位事务、片间被采样插队），换来采样率 13 倍提升。
- `Tetris_Tick` 里 `+= 10 / -= 10` 是按"每次调用 = 10ms"写死的，采样率直接等于游戏节拍：
  整帧刷时 500ms/格实际走 ~7s/格，切片后回到 0.5s/格——这是"手感不流畅"的真因。
- `[PERF]` 各字段：`flush_us/max_us` 单次刷屏耗时、`draw_us` 排版绘制、`fps` 帧率、
  `mpu n/avg/max/late` 实际采样次数与间隔（`late` = 间隔 >15ms 的次数）、
  `loop` 主循环迭代率与其中"轻迭代"数。`main.c` 的 `PERF_TRACE 0` 可整体关掉。
  全部整数定点打印（nano printf 不开 `%f`）。
- 采集：`make rtt`（全量输出，含 `[PERF]`）。脚本在 `tools/rtt/` —— 2026-10-26 从 `build/` 迁出，
  因为 `build/` 被 gitignore，`make clean` 会把脚本一起删掉（踩过一次）。

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
| 旋转 | **短按 PB15**，或敲击板子 | 按键优先推荐（敲击 \|a\| ≥ 1.8g 触发，回落 < 1.2g 再武装 + 200ms 冷却） |

> 双击/硬降已移除：按键瞬间的震动会被体感敲击检测误判成"敲击"，导致双击时旋转与硬降同时触发（2026-10-26 真机反馈）。KEY_DOUBLE 标志位仍由按键状态机产生，但目前无消费方。
>
> 按键震动抑制：任一按键按下（KEY_DOWN）时调用 `Tetris_MuteTap(300)`，300ms 内敲击检测静音，按 PB15 旋转不会被惯性震动多转一次。

> 按键驱动为全功能状态机（移植自教程工程 01-全功能按键：单击/双击/长按/长按连发/按住/按下沿/松开沿 7 标志位，20ms 扫描）。单击要在释放后等 100ms 双击窗确认，所以页面切换/旋转手感会比按下瞬间慢 ~100ms，这是双击识别换来的代价。
| 重开 | 堆到顶后自动重开 | 点阵全亮（整屏红）保持 1.5s → 自动新一局 |
| 手动重开 | **长按 PB15** | 陀螺仪重校准的同时重开一局（校准需保持板子静止 ~1s） |

- 重力：500ms/格起步，每消 5 行快 100ms，下限 150ms
- 计分：1/2/3/4 行 = 1/3/5/8 分（>4 行按 4 行计，8x8 棋盘一次可消超过 4 行）
- 帧格式：`Tetris_GetFrame()` 8 字节，每字节一行，bit7 = 最左列；MAX7219 DIG0..DIG7 直写
- 点阵显示方向若不对（镜像/倒置），改 `Matrix7219_ShowFrame` 里的字节/位映射即可，逻辑层不用动
- host 单测：`tests/test_tetris.c`（19 例，覆盖碰撞/消行/计分/体感判定/重开）

## 红外遥控（v0.11）

0038 一体化红外接收头（OUT→PB12，EXTI 双边沿 + DWT 微秒时间戳，详见 [ADR-005](docs/ADR/005-ir-exti-dwt-decode.md)）。

上电默认 MPU 体感模式；按遥控器 MODE 键在 **IR / MPU** 之间切换（OLED 页 3 底部显示 `CTRL:IR` / `CTRL:MPU`）。

| 功能 | 默认 cmd | 备注 |
|---|---|---|
| 左移 | 0x44 | 按住连发（重复帧） |
| 右移 | 0x43 | 按住连发 |
| 软降 | 0x07 | 按住连发 |
| 旋转 | 0x15 | 不吃重复帧 |
| 硬降 | 0x40 | 不吃重复帧 |
| 模式切换 | 0x09 | 两种模式下都生效 |

**键值校准（必做一次）**：烧录后打开 RTT，按一遍遥控器，从 `[IR] addr=0x.. cmd=0x..` 行读出实际码值，
填进 `User/ir/ir_keymap.h` 重编即可。默认值是常见 21 键迷你遥控，**未实测**。

> 采集窗内验收顺序（顺序影响断言能否拿到）：上电 → 任意按键扫码（默认 MPU 模式下**只出 `[IR] addr=` 行，不产生动作**）
> → MODE 切到 IR → 旋转 / 位移 / 软降 / 硬降各一次 → MODE 切回 MPU。
> `[IR] rotate` / `[IR] harddrop rows=` 只在 IR 模式下产生。

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
- [ADR-004](docs/ADR/004-oled-slice-flush.md) - OLED 整帧刷改分片刷（为什么不引入 RTOS）
- [ADR-005](docs/ADR/005-ir-exti-dwt-decode.md) - 红外接收用 EXTI + DWT 时间戳

## 复用资源

- `MyI2C.c/h` / `My_OLED.c/h` / `Drivers/` / `ThirdParty/RTT/` 全部从 `stm32f103-adc-oled/` 复制
- GCC Makefile 模板结构沿用
- OpenOCD 配置沿用
