---
title: 红外遥控接入 0038 一体化接收头（翻转 / 硬降 / 双模切换）设计
type: spec
tags: [stm32f103, tetris, ir, nec, exti, dwt]
created: 2026-11-01T00:00:00+08:00
modified: 2026-11-01T00:00:00+08:00
description: 为 stm32f103-mpu6050-oled 俄罗斯方块增加 NEC 红外遥控输入源（PB6 EXTI + DWT 时间戳解码）与 MPU/IR 双控制模式
---

# 红外遥控接入设计（v0.11）

## 1. 背景与目标

现有 v0.10 的俄罗斯方块由 MPU6050 体感控制：pitch 倾斜左右移、敲击旋转、roll 软降、PB15 单击旋转 / 长按重校准+重开。本次增加 **0038 一体化红外接收头（2.5mm 直插，38kHz 解调）**，用 NEC 遥控器完成：

- 方块旋转
- 硬降（一路落底并锁定）
- 控制模式切换：**IR 模式** ↔ **MPU 体感模式**
- IR 模式下接管完整玩法：左移 / 右移 / 软降 / 旋转 / 硬降

目标：遥控器单独就能完整玩一局；MPU 模式行为与 v0.10 **零变化**（现有 13 条真机 FR 不回归）。

## 2. 范围

**做**

- NEC 解码器（纯 C，host 可测）+ PB6 EXTI 接收层
- 输入模式状态机（上电默认 MPU）
- `Tetris_HardDrop()`、`Tetris_SetInputMode()`
- 键值映射头文件（默认占位值，RTT 扫码头校准）
- OLED 页 3 模式指示、RTT 日志、host 单测、FSD/ADR/README 文档

**不做（YAGNI）**

- Flash 存储键值 / 运行期学习模式
- 暂停键、音量/亮度遥控
- 新 OLED 页面
- 硬降计分奖励（会改动既有分数语义与单测）
- 5V 供电与电平转换

## 3. 硬件

| 信号 | STM32 引脚 | 说明 |
|---|---|---|
| IR OUT | **PB6** | EXTI6，双边沿中断；空闲高、38kHz 载波期间拉低 |
| IR VCC | 3.3V | 2.7–5.5V 器件可在 3.3V 工作，逻辑电平与 MCU 一致 |
| IR GND | GND | — |

- 供电侧按数据手册：VCC 串 100Ω + 对地并 100µF 电解。
- OUT 开 MCU 内部上拉；长线（>15cm）建议外并 10k 上拉。
- 三脚定义（OUT/GND/VCC）各厂不一致，**以实物丝印为准**；接反不烧，静态量 OUT 应为 ≈3.3V。
- 备选引脚：PB12 / PB13 / PB14（同为 EXTI 能力，PB6 被占用时替换，仅改 `ir_rx.c` 顶部宏与 AFIO 端口）。

**引脚冲突核查**：PA0 KEY0、PB15 KEY1、PA4/5/7 MAX7219、PA6 ADC、PB8/9 OLED、PB10/11 MPU、PA13/14 SWD。PB6 全项目未占用。

## 4. NEC 判据

| 段 | 低电平 | 高电平 |
|---|---|---|
| 前导（数据帧） | 9ms | 4.5ms |
| 前导（重复帧） | 9ms | 2.25ms |
| 位 `0` | 560µs | 560µs |
| 位 `1` | 560µs | 1690µs |
| 帧尾 | — | 560µs 低后转空闲 |

- 32 位帧序：地址(8) → ~地址(8) → 命令(8) → ~命令(8)，**LSB 先发**。
- 高位宽判定容差 ±30%（实现取区间：低 400–900µs、高 0 位 400–900µs、1 位 1400–2100µs、前导高 3.6–5.2ms、重复高 1.9–2.6ms）。
- 毛刺：脉冲 < 350µs 丢弃并复位状态机。
- 空闲：边沿间隔 > 12ms 视为新帧起点，先清部分帧。
- 校验：**动作门槛只看命令字节 `cmd ^ ~cmd == 0xFF`**（扩展 NEC 遥控器的 16 位地址会让 `addr` 补码不成立，若用地址校验会全盘丢弃）。地址补码成立 → 标记 `standard`；不成立 → 标记 `extended`，两者都允许动作。命令补码不成立才丢弃动作，但**仍打印原始 32 位**（带 `BAD` 标记，供扫码与排障）。
- 重复帧两种形态都归为 REPEAT：标准重复码（9ms+2.25ms），或同 `(addr, cmd)` 在 150ms 内再次到达。

## 5. 架构

```
PB6 边沿 ──► EXTI9_5_IRQHandler ──► ir_rx.c
                                     │ 读 DWT->CYCCNT（72MHz，13.9ns）算 dt
                                     ▼
                              ir_decode.c（纯 C，无 HAL）
                                     │ 状态机 → 完整帧
                                     ▼
                              4 深度环形队列
                                     │
主循环 ──► IrRx_Poll(&ev) ──► 键映射 (ir_keymap.h) ──► Tetris_* 动作
                                     └─► RTT 日志
```

**为什么不用轮询**：OLED 软件 I2C 整帧刷阻塞最坏 ~13ms，而 NEC 一个 `0` 位仅 1.12ms，轮询必漏码。

**为什么不用 TIM 输入捕获**：需启用 `HAL_TIM_MODULE` 并加入 `hal_tim.c`/`hal_tim_ex.c`（flash +3~5KB、代码 +60 行、CCR 溢出处理），换来的是对 560µs 位宽无意义的精度提升。EXTI 中断延迟抖动 ~1µs，对位宽判定误差 0.2%。DWT CYCCNT 已为软件 I2C 延时启用。

**ISR 纪律**：只读时间戳、跑解码状态机、入队。不做动作派发、不做字符串打印、不做阻塞调用。

**DWT 回绕**：CYCCNT 32 位 @72MHz 每 59.6s 回绕，全部用无符号差值，天然正确。

## 6. 接口契约

```c
/* ---------- ir_decode.h（纯 C，主机可测） ---------- */
#define IR_FRAME_OK      0
#define IR_FRAME_DATA    1   /* 完整数据帧 */
#define IR_FRAME_REPEAT  2   /* 重复帧 */

typedef struct {
    uint32_t raw;     /* 原始 32 位（LSB 先收，按 NEC 顺序拼） */
    uint8_t  addr;    /* raw[7:0]   */
    uint8_t  cmd;     /* raw[23:16] */
    uint8_t  valid;   /* 1 = 命令补码校验通过（动作门槛） */
    uint8_t  standard;/* 1 = 地址补码也成立（标准 NEC），0 = 扩展 NEC，仅影响日志 */
    uint8_t  kind;    /* IR_FRAME_DATA / IR_FRAME_REPEAT */
} IrFrame_t;

void    IrDecode_Reset(void);
/* 喂一个边沿：level = 边沿后的电平（0=低 1=高），dt_us = 距上一沿的微秒数
 * 返回 1 = 产出一帧（写入 *out），0 = 无输出 */
uint8_t IrDecode_Edge(uint8_t level, uint32_t dt_us, IrFrame_t *out);

/* ---------- ir_rx.h（硬件层） ---------- */
#define IR_EV_DATA     1
#define IR_EV_REPEAT   2

typedef struct {
    uint8_t  kind;      /* IR_EV_DATA / IR_EV_REPEAT */
    uint8_t  addr;
    uint8_t  cmd;
    uint8_t  valid;     /* 1 = 命令补码校验通过（动作门槛） */
    uint8_t  standard;  /* 1 = 地址补码也成立（扩展 NEC 时为 0） */
    uint32_t raw;       /* 原始 32 位，供 RTT 扫码/排障打印 */
} IrEvent_t;

void    IrRx_Init(void);                 /* PB6 EXTI 双边沿 + 优先级 1 */
uint8_t IrRx_Poll(IrEvent_t *ev);        /* 主循环取事件，1=有 */

/* ---------- tetris.h 新增 ---------- */
uint8_t Tetris_SoftDrop(void);           /* 下移一格；撞底则锁定并出下一块；1=下移成功 0=已锁定/结束 */
uint8_t Tetris_HardDrop(void);           /* 落到底 + 锁定 + 出下一块；返回掉落格数 */
typedef enum { TETRIS_INPUT_SENSOR = 0, TETRIS_INPUT_IR = 1 } TetrisInputMode;
void    Tetris_SetInputMode(TetrisInputMode mode);
TetrisInputMode Tetris_GetInputMode(void);
```

**`Tetris_SetInputMode(IR)` 语义**：`Tetris_Tick` 跳过倾斜侧移、敲击旋转、roll 软降三项判定，**保留**重力下落与游戏结束倒计时，并重置 `s_tilt_armed = 1`、`s_tap_pressed = 0`、`s_tap_cooldown_ms = 0`。**两个切换方向都做同样的复位**，保证每次切换都从"已武装 / 未按下 / 无冷却"的干净状态起步：IR 期间累积的误触发/按下态不会被带回 SENSOR 模式把横移卡死。

**`Tetris_HardDrop()` 语义**：循环 `y+1` 至碰撞前，然后强制 `lock_piece()`；游戏结束状态返回 0 不动作；返回掉落格数（日志用）。不计分。

## 7. 主循环派发

```
每轮主循环：
  IrRx_Poll(&ev)
  若 ev.kind == DATA：
      RTT: [IR] addr=0xAA cmd=0xCC raw=0x........ [BAD]
      查 ir_keymap.h（只匹配 cmd）
      MODE 键  → 翻转 s_ctrl_mode；Tetris_SetInputMode()；RTT: [IR] mode=IR|MPU
      其余键  → 仅当模式为 IR 时生效：
                  LEFT/RIGHT → Tetris_ShiftLeft/ShiftRight
                  DOWN       → Tetris_SoftDrop（下移一格；撞底即锁定出块）
                  ROTATE     → Tetris_Rotate
                  HARDDROP   → Tetris_HardDrop；RTT: [IR] harddrop rows=N
  若 ev.kind == REPEAT（且模式为 IR）：
      仅对 LEFT/RIGHT/DOWN 生效（~9 次/秒连发）
      对 ROTATE / HARDDROP / MODE 忽略
```

- **MODE 键在两种模式下都生效**；其余动作键仅在 IR 模式生效。MPU 模式保持纯体感，不引入混合输入的歧义。
- 上电默认 **MPU 模式**。
- 软降：`DOWN` 键直接执行一格下移（撞底即 `lock_piece`），不走 `Tetris_Tick` 的重力累加器，避免连发时与重力周期互相干扰。

## 8. 键位映射（占位，扫码后替换）

`User/ir/ir_keymap.h`：

```c
/* 默认值 = 常见 NEC 21 键迷你遥控；实战以 RTT 扫出的 cmd 为准 */
#define IR_KEY_LEFT       0x44
#define IR_KEY_RIGHT      0x43
#define IR_KEY_DOWN       0x07
#define IR_KEY_ROTATE     0x15
#define IR_KEY_HARDDROP   0x40
#define IR_KEY_MODE       0x09
```

**扫码流程**：固件只做"解码即打印"。烧录后按一遍遥控器，从 RTT 日志读 `cmd=0x??`，填进上表重编即完成校准。逻辑代码不动。

## 9. 显示

- OLED 页 3（俄罗斯方块副屏）底部一行随模式变化：
  - MPU：`CTRL:MPU TILT+TAP`
  - IR：`CTRL:IR  REMOTE`
  - 游戏结束时仍优先显示 `NEW GAME...`
- RTT：`[IR] init pin=PB6` / 每帧解码一行 / 模式切换一行 / 硬降格数一行。
- MAX7219 盘面不做模式提示，避免打断游戏画面。

## 10. 功能需求（新增 FR）

| 编号 | 需求 | 验收断言 |
|---|---|---|
| FR-IR-01 | 上电初始化红外接收，启动日志标明引脚 | `[IR] init pin=PB6` |
| FR-IR-02 | 每收到一个 NEC 帧打印地址/命令/原始值 | `[IR] addr=0x.. cmd=0x.. raw=0x........` |
| FR-IR-03 | MODE 键在两种模式下都能切换 | `[IR] mode=IR` / `[IR] mode=MPU` 交替出现 |
| FR-IR-04 | IR 模式下旋转 / 硬降可用 | `[IR] rotate` / `[IR] harddrop rows=N` |
| FR-IR-05 | IR 模式下体感失效、重力仍走 | 倾斜板子方块不横移；方块自动下落 |
| FR-IR-06 | MPU 模式下遥控动作键不动作（MODE 除外） | MPU 模式下按旋转键无 `[IR] rotate` |
| FR-IR-07 | 重复帧只对位移/软降连发 | 按住旋转键只出转一次 |

NFR-05：ISR 内不得出现 `SEGGER_RTT_*`、`snprintf`、`HAL_Delay`；flash 增量 ≤ 4KB；RAM 增量 ≤ 64B。

## 11. 测试计划

**Host 单测（`make test`）**

- `tests/test_ir_decode.c`（新）：前导识别、0/1 位判定与容差边界、32 位组帧序（LSB 先）、双补码校验失败丢弃、重复帧（标准重复码 + 同码重发两种形态）、<350µs 毛刺、>12ms 空闲重同步、半帧中断后重同步。
- `tests/test_tetris.c`（补）：
  - `Tetris_HardDrop` 落到底并锁定出新块；游戏结束时返回 0。
  - `Tetris_SoftDrop` 每次下移一格，撞底时锁定并出下一块。
  - `TETRIS_INPUT_IR` 下喂倾斜/敲击序列不产生横移与旋转，但重力仍按周期下落。
  - IR → SENSOR 切回后，`s_tilt_armed` 已复位，不会在首个 tick 立刻横移。

**真机验收（verify.py 链路）**

| FR | 人工动作 |
|---|---|
| FR-IR-01 | 上电 |
| FR-IR-02 | 遥控器随意按键 |
| FR-IR-03 | 按 MODE 键两次 |
| FR-IR-04 | IR 模式下按旋转键、硬降键 |
| FR-IR-05 | IR 模式下倾斜板子 |
| FR-IR-06 | 切回 MPU 模式后按旋转键 |
| 既有 4 条 | PB15 长按 CAL → PA0 短按 ×3 → PA0 长按关屏（顺序不可变） |

## 12. 风险与对策

| 风险 | 对策 |
|---|---|
| 遥控器码值未知 | RTT 扫码头；只改 `ir_keymap.h` |
| 0038 三脚定义不一致 | 文档明确"以丝印为准"，静态量 OUT ≈3.3V |
| 中断破坏软件 I2C 时序 | 软 I2C 只有最小时间约束，ISR 十几 µs 只拉长位周期；ISR 不做派发/打印 |
| 重复帧形态不一 | 标准重复码 + 同码 150ms 窗口，两条都认 |
| 遥控器按键抖动产生连帧 | 旋转/硬降/模式键不吃 REPEAT；位移类连发是期望行为 |
| 5V 供电 | 强制 3.3V |

## 13. 发布与文档

- 版本 **v0.11**；发布记录沿用既有 `docs/release` 惯例，真机 13/13 基线叠加 FR-IR-01~07。
- `docs/FSD.md`：补 FR-IR-01~07、NFR-05。
- `docs/ADR/005-ir-exti-dwt-decode.md`：记录"否决 TIM 输入捕获（精度过剩、引入 HAL_TIM 依赖）"与"否决轮询（OLED 阻塞 13ms 必漏码）"。
- `README.md`：接线表加 IR 三行、遥控键位表、扫码校准步骤、故障排查（无输出/码值不符/模式不切）。

## 14. 假设

1. 用户遥控器为 NEC 协议（0038 接收头只解 38kHz 载波，协议侧若是 RC5/RC6 则需换解码器——届时以 RTT 日志零输出为判据，改用另一套解码）。
2. PB6 在实物板上未被额外占用（备选 PB12/PB13/PB14）。
3. 遥控器具备至少 6 个可用键。
