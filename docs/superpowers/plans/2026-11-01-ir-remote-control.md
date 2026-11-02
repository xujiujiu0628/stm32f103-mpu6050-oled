# 红外遥控接入实施计划（v0.11）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 stm32f103-mpu6050-oled 的俄罗斯方块加 NEC 红外遥控输入源，实现遥控翻转 / 硬降 / 左右移 / 软降 / 以及 IR ↔ MPU 双控制模式切换。

**Architecture:** 解码分两层——`ir_decode.c` 是零硬件依赖的纯状态机（边沿+间隔 → 帧），`ir_rx.c` 只负责 PB6 双边沿 EXTI、读 DWT->CYCCNT 换算微秒、把帧压进 4 深度环形队列。主循环 `IrRx_Poll()` 取事件，查 `ir_keymap.h` 映射表派发到 `Tetris_*`。模式切换由 `Tetris_SetInputMode()` 承担，IR 模式下 `Tetris_Tick` 只跑重力与结束计时。

**Tech Stack:** STM32F103C8T6 / GCC arm-none-eabi 10.2021.10 / HAL（GPIO+EXTI，不引入 HAL_TIM）/ SEGGER RTT / MAX7219 / SSD1306 / MSYS2 mingw64 gcc 做 host 单测。

**Spec:** `docs/superpowers/specs/2026-11-01-ir-remote-control-design.md`

## Global Constraints

- 工作目录：`D:/claude/hw-projects/stm32f103-mpu6050-oled`（git 仓库，分支 master）。
- 引脚：IR OUT = **PB6**，EXTI9_5，NVIC 优先级 **1**（SysTick 保持 0）；IR VCC = **3.3V**。
- **不改** `[BOOT] stm32f103-mpu6050-oled v0.1` 横幅——`expectations.json` 的 FR-BOOT-01 断言这段原文。
- 上电默认 **MPU（SENSOR）模式**；MODE 键两模式下都生效，其余动作键仅在 IR 模式生效。
- 重复帧（`IR_EV_REPEAT`）只对左/右/软降生效；旋转 / 硬降 / MODE 忽略重复帧。
- ISR 内禁止 `SEGGER_RTT_*`、`snprintf`、`HAL_Delay`、任何循环等待。
- 构建必须**零警告**（`-Wall`）；flash 增量 ≤ 4KB，RAM 增量 ≤ 96B（基线：text 37432 / data 488 / bss 3608）。
- 头文件用 include guard（`__IR_DECODE_H` 风格，与项目一致）；注释与提交信息用中文，提交信息用 conventional commit 前缀（`feat:` / `test:` / `docs:`）。
- 每个 Task 结束时 `git status --short` 为空。
- 所有 Windows 路径在 shell 中用正斜杠。

## File Structure

| 文件 | 责任 | 动作 |
|---|---|---|
| `User/ir/ir_decode.h` | 解码器接口 + `IrFrame_t` | 新建 |
| `User/ir/ir_decode.c` | NEC 边沿状态机（纯 C，无 HAL） | 新建 |
| `User/ir/ir_rx.h` | 接收层接口 + `IrEvent_t` | 新建 |
| `User/ir/ir_rx.c` | PB6 EXTI + DWT 时间戳 + 环形队列 | 新建 |
| `User/ir/ir_keymap.h` | 命令字节 → 功能映射 | 新建 |
| `tests/test_ir_decode.c` | 解码器 host 单测 | 新建 |
| `User/tetris/tetris.h` / `.c` | 新增 SoftDrop / HardDrop / 输入模式 | 修改 |
| `tests/test_tetris.c` | 新增 4 组用例 | 修改 |
| `User/main.c` | `IrRx_Init` + 事件派发 + 模式状态 | 修改 |
| `User/oled_view/oled_view.c` | 页 3 底部模式行 | 修改 |
| `User/stm32f1xx_it.c` | `EXTI9_5_IRQHandler` | 修改 |
| `Makefile` | 源文件 / include / host 测试目标 | 修改 |
| `.workbench/expectations.json` | FR-IR-01~04 机器断言 | 修改 |
| `docs/FSD.md` / `docs/ADR/005-*.md` / `README.md` | 规格、决策记录、接线与验收 | 修改/新建 |

---

### Task 1: NEC 纯解码器

**Files:**
- Create: `User/ir/ir_decode.h`
- Create: `User/ir/ir_decode.c`
- Test: `tests/test_ir_decode.c`

**Interfaces:**
- Consumes: 无（不依赖 HAL / CMSIS）
- Produces:
  - `typedef struct { uint32_t raw; uint8_t addr; uint8_t cmd; uint8_t valid; uint8_t standard; uint8_t kind; } IrFrame_t;`
  - `void IrDecode_Reset(void);`
  - `uint8_t IrDecode_Edge(uint8_t level, uint32_t dt_us, IrFrame_t *out);` — 返回 1 表示产出一帧
  - `#define IR_FRAME_DATA 1` / `#define IR_FRAME_REPEAT 2`

- [ ] **Step 1: 写失败测试 `tests/test_ir_decode.c`**

```c
/**
  ******************************************************************************
  * @file    test_ir_decode.c
  * @brief   NEC 解码器 host 单测：合成边沿序列驱动（不碰硬件）
  *
  *  编译运行（Git Bash，工程根）:
  *    gcc -std=c99 -Wall -Wextra -IUser/ir -o build/test_ir_decode.exe \
  *        tests/test_ir_decode.c User/ir/ir_decode.c && ./build/test_ir_decode.exe
  ******************************************************************************
  */
#include <stdio.h>
#include "ir_decode.h"

static int s_fail = 0;
static int s_total = 0;

#define CHECK(cond, name) do {                                   \
    s_total++;                                                   \
    if (!(cond)) { s_fail++; printf("  [FAIL] %s\n", name); }    \
} while (0)

#define EQ(a, b, name) CHECK((a) == (b), name)

static IrFrame_t s_out;

/* 喂一个边沿：level = 边沿后的电平 */
static uint8_t edge(uint8_t level, uint32_t us)
{
    return IrDecode_Edge(level, us, &s_out);
}

/* 发一个 32 位原始帧（bit0 先发），返回 1 = 产出了帧 */
static uint8_t send_raw(uint32_t raw)
{
    int i;
    edge(0, 20000);                 /* 空闲后下降：前导低起点 */
    edge(1, 9000);                  /* 前导低 9ms 结束 */
    if (edge(0, 4500)) return 1;    /* 前导高 4.5ms 结束 → 数据相位（此处不应产出） */
    for (i = 0; i < 32; i++)
    {
        edge(1, 560);                                   /* 数据位低 560us 结束 */
        if (edge(0, ((raw >> i) & 1U) ? 1690U : 560U))   /* 高电平结束 = 下一位开始 */
            return 1;                                   /* 第 32 位在此产出 */
    }
    return 0;
}

/* 按 NEC 双补码规则发一个标准数据帧 */
static uint8_t send_data(uint8_t addr, uint8_t cmd)
{
    uint32_t raw = (uint32_t)addr
                 | ((uint32_t)(uint8_t)~addr << 8)
                 | ((uint32_t)cmd << 16)
                 | ((uint32_t)(uint8_t)~cmd << 24);
    return send_raw(raw);
}

/* 标准重复帧：9ms 低 + 2.25ms 高 */
static uint8_t send_repeat(void)
{
    edge(0, 20000);
    edge(1, 9000);
    return edge(0, 2250);
}

int main(void)
{
    printf("== NEC 解码器 ==\n");

    /* 1. 完整数据帧 + 字段还原 */
    IrDecode_Reset();
    EQ(send_data(0x00, 0x45), 1, "数据帧产出 1 帧");
    EQ(s_out.kind, IR_FRAME_DATA, "kind=DATA");
    EQ(s_out.addr, 0x00, "addr=0x00");
    EQ(s_out.cmd, 0x45, "cmd=0x45");
    EQ(s_out.valid, 1, "命令补码校验通过");
    EQ(s_out.standard, 1, "地址补码校验通过");
    EQ(s_out.raw, 0xBA45FF00u, "raw 位序正确");

    /* 2. LSB 先发：反向位序会被这条抓住 */
    EQ(send_data(0x01, 0x80), 1, "第二帧产出");
    EQ(s_out.raw, 0x7F80FE01u, "raw LSB 先发");
    EQ(s_out.addr, 0x01, "addr=0x01");
    EQ(s_out.cmd, 0x80, "cmd=0x80");

    /* 3. 扩展 NEC：地址补码不成立 → standard=0，但动作门槛 valid 仍为 1 */
    EQ(send_raw(0xBA450012u), 1, "扩展 NEC 仍产出");
    EQ(s_out.valid, 1, "命令补码通过");
    EQ(s_out.standard, 0, "地址补码不成立标记为扩展 NEC");

    /* 4. 命令补码失败：仍产出帧，valid=0（供扫码与排障） */
    EQ(send_raw(0x0045FF00u), 1, "坏命令帧仍产出");
    EQ(s_out.valid, 0, "命令补码失败 valid=0");

    /* 5. 标准重复帧 */
    IrDecode_Reset();
    EQ(send_data(0x00, 0x45), 1, "先发一帧数据");
    EQ(send_repeat(), 1, "重复帧产出");
    EQ(s_out.kind, IR_FRAME_REPEAT, "kind=REPEAT");
    EQ(s_out.cmd, 0x45, "重复帧沿用上一个 cmd");

    /* 6. 遥控器重发整帧（同码 150ms 内）→ 也算 REPEAT */
    IrDecode_Reset();
    EQ(send_data(0x00, 0x46), 1, "首帧");
    EQ(send_data(0x00, 0x46), 1, "重发帧");
    EQ(s_out.kind, IR_FRAME_REPEAT, "同码重发 → REPEAT");

    /* 7. 超过 150ms 的同码再按 → 算新的一次 DATA */
    IrDecode_Reset();
    EQ(send_data(0x00, 0x46), 1, "首帧");
    edge(0, 200000);                       /* 空闲 200ms */
    EQ(send_data(0x00, 0x46), 1, "隔 200ms 再按");
    EQ(s_out.kind, IR_FRAME_DATA, "超窗口同码 = DATA");

    /* 8. 帧中毛刺（<350us 低）丢弃整帧 */
    {
        uint8_t got = 0;
        int i;
        IrDecode_Reset();
        s_out.kind = 0;
        edge(0, 20000);
        edge(1, 9000);
        edge(0, 4500);
        for (i = 0; i < 10; i++)
        {
            edge(1, 560);
            got |= edge(0, ((0xBA45FF00u >> i) & 1U) ? 1690U : 560U);
        }
        edge(1, 200);                      /* 200us 毛刺：低电平太短 */
        for (i = 10; i < 32; i++)
        {
            edge(1, 560);
            got |= edge(0, ((0xBA45FF00u >> i) & 1U) ? 1690U : 560U);
        }
        EQ(got, 0, "毛刺后整帧丢弃");
    }

    /* 9. 半帧后长空闲 → 重新同步 */
    {
        int i;
        IrDecode_Reset();
        edge(0, 20000);
        edge(1, 9000);
        edge(0, 4500);
        for (i = 0; i < 8; i++) { edge(1, 560); edge(0, 560); }
        edge(0, 20000);                    /* 半帧后进空闲 */
        EQ(send_data(0x00, 0x45), 1, "半帧后重同步");
        EQ(s_out.kind, IR_FRAME_DATA, "重同步产出 DATA");
    }

    /* 10. 位宽边界：低位/高位都在窗口边缘时仍判对 */
    {
        int i;
        uint8_t got = 0;
        IrDecode_Reset();
        edge(0, 20000);
        edge(1, 9000);
        edge(0, 4500);
        for (i = 0; i < 32; i++) { edge(1, 400); got |= edge(0, 900); }   /* 低 400 / 高 900 */
        EQ(got, 1, "边界帧产出");
        EQ(s_out.raw, 0x00000000u, "低 400/高 900 全判 0");

        IrDecode_Reset();
        got = 0;
        edge(0, 20000);
        edge(1, 9000);
        edge(0, 4500);
        for (i = 0; i < 32; i++) { edge(1, 900); got |= edge(0, 1400); }  /* 低 900 / 高 1400 */
        EQ(got, 1, "上边界帧产出");
        EQ(s_out.raw, 0xFFFFFFFFu, "低 900/高 1400 全判 1");
    }

    /* 11. 位低电平超窗（1200us）→ 整帧丢弃 */
    {
        int i;
        uint8_t got = 0;
        IrDecode_Reset();
        edge(0, 20000);
        edge(1, 9000);
        edge(0, 4500);
        for (i = 0; i < 32; i++) { edge(1, 1200); got |= edge(0, 560); }
        EQ(got, 0, "位低电平超窗丢弃整帧");
    }

    /* 12. 没有前导的重复帧 → 不产出 */
    IrDecode_Reset();
    EQ(send_repeat(), 0, "孤立重复帧不产出");

    printf("== 结果: %d/%d 通过 ==\n", s_total - s_fail, s_total);
    return s_fail ? 1 : 0;
}
```

- [ ] **Step 2: 运行测试确认失败**

Run:
```bash
cd /d/claude/hw-projects/stm32f103-mpu6050-oled
export PATH="/c/msys64/mingw64/bin:$PATH"
gcc -std=c99 -Wall -Wextra -IUser/ir -o build/test_ir_decode.exe tests/test_ir_decode.c User/ir/ir_decode.c
```
Expected: 编译失败，`ir_decode.h: No such file or directory`。

- [ ] **Step 3: 写 `User/ir/ir_decode.h`**

```c
/**
  ******************************************************************************
  * @file    ir_decode.h
  * @brief   NEC 红外解码器（纯 C，无 HAL / 无硬件依赖，host 可测）
  *
  *  输入：接收头输出的边沿（电平 + 距上一沿的微秒数）
  *  输出：解出的 32 位帧 / 重复帧
  ******************************************************************************
  */
#ifndef __IR_DECODE_H
#define __IR_DECODE_H

#include <stdint.h>

/* 帧种类 */
#define IR_FRAME_DATA    1
#define IR_FRAME_REPEAT  2

typedef struct {
    uint32_t raw;      /* 原始 32 位，bit0 = 第一个收到的位（NEC LSB 先发） */
    uint8_t  addr;     /* raw[7:0]   */
    uint8_t  cmd;      /* raw[23:16] */
    uint8_t  valid;    /* 1 = 命令补码校验通过（动作门槛） */
    uint8_t  standard; /* 1 = 地址补码也成立（标准 NEC） */
    uint8_t  kind;     /* IR_FRAME_DATA / IR_FRAME_REPEAT */
} IrFrame_t;

/**
 * @brief  复位解码器（含"上一帧"记录，重复帧窗口一并清空）
 */
void IrDecode_Reset(void);

/**
 * @brief  喂一个边沿
 * @param  level  边沿后的电平（0=低 1=高）
 * @param  dt_us  距上一沿的微秒数
 * @param  out    产出帧写入处（返回 1 时有效）
 * @retval 1=产出一帧 0=无输出
 */
uint8_t IrDecode_Edge(uint8_t level, uint32_t dt_us, IrFrame_t *out);

#endif /* __IR_DECODE_H */
```

- [ ] **Step 4: 写 `User/ir/ir_decode.c`**

```c
/**
  ******************************************************************************
  * @file    ir_decode.c
  * @brief   NEC 协议边沿状态机
  *
  * @时序    前导 9ms 低 + 4.5ms 高（重复帧高 2.25ms）；
  *          位 0 = 560us 低 + 560us 高；位 1 = 560us 低 + 1690us 高；
  *          32 位 = 地址 / ~地址 / 命令 / ~命令，LSB 先发。
  * @容差    各段取标称值 ±30% 窗口
  * @重复帧  标准重复码（9ms+2.25ms），或同 (addr,cmd) 在 150ms 内重发整帧
  ******************************************************************************
  */
#include "ir_decode.h"

/* 判定窗口（us） */
#define IR_LEADER_LOW_MIN     7000U
#define IR_LEADER_LOW_MAX    11000U
#define IR_LEADER_HIGH_MIN    3600U
#define IR_LEADER_HIGH_MAX    5200U
#define IR_REPEAT_HIGH_MIN    1900U
#define IR_REPEAT_HIGH_MAX    2600U
#define IR_BIT_LOW_MIN         400U
#define IR_BIT_LOW_MAX         900U
#define IR_BIT0_HIGH_MIN       400U
#define IR_BIT0_HIGH_MAX       900U
#define IR_BIT1_HIGH_MIN      1400U
#define IR_BIT1_HIGH_MAX      2100U
#define IR_GLITCH_MAX          350U
#define IR_IDLE_GAP_MIN      12000U
#define IR_REPEAT_WINDOW_US 150000U

typedef enum { IR_ST_IDLE = 0, IR_ST_LEADER, IR_ST_DATA } IrState_t;

static IrState_t s_st;
static uint32_t  s_raw;
static uint8_t   s_bitcnt;
static uint32_t  s_since_frame_us;          /* 距上一完整帧的累计时间 */
static uint8_t   s_last_addr, s_last_cmd, s_have_last;

void IrDecode_Reset(void)
{
    s_st = IR_ST_IDLE;
    s_raw = 0;
    s_bitcnt = 0;
    s_since_frame_us = 0;
    s_have_last = 0;
    s_last_addr = 0;
    s_last_cmd = 0;
}

uint8_t IrDecode_Edge(uint8_t level, uint32_t dt_us, IrFrame_t *out)
{
    if (dt_us > 0xFFFFFFFFu - s_since_frame_us) s_since_frame_us = 0xFFFFFFFFu;
    else                                        s_since_frame_us += dt_us;

    if (level == 0U)
    {
        /* 下降沿：刚结束的是一段高电平，长度 = dt_us */
        if (dt_us >= IR_IDLE_GAP_MIN)
        {
            /* 空闲边界：这是新前导低电平的起点 */
            s_st = IR_ST_LEADER;
            s_raw = 0;
            s_bitcnt = 0;
            return 0;
        }

        if (s_st == IR_ST_LEADER)
        {
            if (dt_us >= IR_LEADER_HIGH_MIN && dt_us <= IR_LEADER_HIGH_MAX)
            {
                s_st = IR_ST_DATA;
                s_raw = 0;
                s_bitcnt = 0;
            }
            else if (dt_us >= IR_REPEAT_HIGH_MIN && dt_us <= IR_REPEAT_HIGH_MAX)
            {
                s_st = IR_ST_IDLE;
                if (!s_have_last) return 0;
                out->raw      = 0;
                out->addr     = s_last_addr;
                out->cmd      = s_last_cmd;
                out->valid    = 1;
                out->standard = 1;
                out->kind     = IR_FRAME_REPEAT;
                s_since_frame_us = 0;
                return 1;
            }
            else
            {
                s_st = IR_ST_IDLE;
            }
            return 0;
        }

        if (s_st == IR_ST_DATA)
        {
            if (dt_us < IR_BIT0_HIGH_MIN || dt_us > IR_BIT1_HIGH_MAX)
            {
                s_st = IR_ST_IDLE;
                s_raw = 0;
                s_bitcnt = 0;
                return 0;
            }
            if (dt_us >= IR_BIT1_HIGH_MIN) s_raw |= (1UL << s_bitcnt);
            s_bitcnt++;
            if (s_bitcnt < 32U) return 0;

            {
                uint8_t a  = (uint8_t)(s_raw & 0xFFU);
                uint8_t na = (uint8_t)((s_raw >> 8) & 0xFFU);
                uint8_t c  = (uint8_t)((s_raw >> 16) & 0xFFU);
                uint8_t nc = (uint8_t)((s_raw >> 24) & 0xFFU);

                s_st = IR_ST_IDLE;
                s_bitcnt = 0;
                out->raw      = s_raw;
                out->addr     = a;
                out->cmd      = c;
                out->standard = (uint8_t)(((a ^ na) == 0xFFU) ? 1U : 0U);
                out->valid    = (uint8_t)(((c ^ nc) == 0xFFU) ? 1U : 0U);
                out->kind     = IR_FRAME_DATA;
                if (out->valid && s_have_last && c == s_last_cmd && a == s_last_addr
                    && s_since_frame_us <= IR_REPEAT_WINDOW_US)
                {
                    out->kind = IR_FRAME_REPEAT;
                }
                if (out->valid)
                {
                    s_last_addr = a;
                    s_last_cmd  = c;
                    s_have_last = 1;
                }
                s_since_frame_us = 0;
                return 1;
            }
        }
        return 0;
    }

    /* 上升沿：刚结束的是一段低电平 */
    if (dt_us > IR_IDLE_GAP_MIN) { s_st = IR_ST_IDLE; return 0; }
    if (dt_us < IR_GLITCH_MAX)   { s_st = IR_ST_IDLE; return 0; }
    if (s_st == IR_ST_LEADER &&
        (dt_us < IR_LEADER_LOW_MIN || dt_us > IR_LEADER_LOW_MAX))
    {
        s_st = IR_ST_IDLE;
    }
    else if (s_st == IR_ST_DATA &&
             (dt_us < IR_BIT_LOW_MIN || dt_us > IR_BIT_LOW_MAX))
    {
        s_st = IR_ST_IDLE;
    }
    return 0;
}
```

- [ ] **Step 5: 运行测试确认全绿**

Run:
```bash
cd /d/claude/hw-projects/stm32f103-mpu6050-oled
export PATH="/c/msys64/mingw64/bin:$PATH"
gcc -std=c99 -Wall -Wextra -IUser/ir -o build/test_ir_decode.exe tests/test_ir_decode.c User/ir/ir_decode.c && ./build/test_ir_decode.exe
```
Expected: `== 结果: N/N 通过 ==`，零 `[FAIL]`，零编译警告。

- [ ] **Step 6: 提交**

```bash
cd /d/claude/hw-projects/stm32f103-mpu6050-oled
git add User/ir/ir_decode.h User/ir/ir_decode.c tests/test_ir_decode.c
git commit -m "feat(ir): NEC 边沿解码器（纯 C 状态机 + 12 组 host 单测）"
```

---

### Task 2: Tetris 硬降 / 软降 / 输入模式

**Files:**
- Modify: `User/tetris/tetris.h`
- Modify: `User/tetris/tetris.c`
- Test: `tests/test_tetris.c`

**Interfaces:**
- Consumes: 现有 `tetris.c` 内部 `collide()` / `lock_piece()` / `Tetris_Reset()` / `Tetris_Tick()`
- Produces:
  - `uint8_t Tetris_SoftDrop(void);` 1=下移一格，0=已锁定/游戏结束
  - `uint8_t Tetris_HardDrop(void);` 返回掉落格数
  - `typedef enum { TETRIS_INPUT_SENSOR = 0, TETRIS_INPUT_IR = 1 } TetrisInputMode;`
  - `void Tetris_SetInputMode(TetrisInputMode mode);`
  - `TetrisInputMode Tetris_GetInputMode(void);`

- [ ] **Step 1: 在 `tests/test_tetris.c` 末尾追加失败测试**

先在文件头 include 区加一行（现有只有 `stdio.h` / `math.h` / `tetris.h`）：

```c
#include <string.h>
```

再在 `printf("== 结果: ...")` 之前插入：

```c
    /* --- 硬降：一路落底 + 锁定出新块 --- */
    {
        uint8_t rows;
        Tetris_SetInputMode(TETRIS_INPUT_SENSOR);
        Tetris_Reset();
        set_board((const uint8_t[8]){0,0,0,0,0,0,0,0});
        rows = Tetris_HardDrop();
        CHECK(rows >= 5, "硬降落底格数 >= 5");
        CHECK(Tetris_GetFrame()[7] != 0, "硬降后底行有锁定块");
        CHECK(!Tetris_IsGameOver(), "硬降后未判负");
    }

    /* --- 软降：逐格下移，到底锁定 --- */
    {
        int n = 0;
        Tetris_SetInputMode(TETRIS_INPUT_SENSOR);
        Tetris_Reset();
        set_board((const uint8_t[8]){0,0,0,0,0,0,0,0});
        while (Tetris_SoftDrop() && n < 20) n++;
        CHECK(n >= 5, "软降逐格下移");
        CHECK(Tetris_GetFrame()[7] != 0, "软降到底锁定出块");
    }

    /* --- IR 模式：倾斜/敲击不动作，重力仍在 --- */
    {
        uint8_t snap[8];
        int i, same;
        Tetris_SetInputMode(TETRIS_INPUT_IR);
        Tetris_Reset();
        set_board((const uint8_t[8]){0,0,0,0,0,0,0,0});

        memcpy(snap, Tetris_GetFrame(), 8);
        tick_n(-40.0f, 0, 0, 1.0f, 3);          /* 30ms：不足以触发重力 */
        same = 1;
        for (i = 0; i < 8; i++) same &= (Tetris_GetFrame()[i] == snap[i]);
        CHECK(same, "IR 模式：倾斜不横移");

        memcpy(snap, Tetris_GetFrame(), 8);
        tick_n(0.0f, 0.0f, 0.0f, 2.2f, 2);      /* 敲击尖峰 */
        same = 1;
        for (i = 0; i < 8; i++) same &= (Tetris_GetFrame()[i] == snap[i]);
        CHECK(same, "IR 模式：敲击不旋转");

        memcpy(snap, Tetris_GetFrame(), 8);
        tick_n(0.0f, 0, 0, 1.0f, 120);          /* 1.2s 重力 */
        same = 1;
        for (i = 0; i < 8; i++) same &= (Tetris_GetFrame()[i] == snap[i]);
        CHECK(!same, "IR 模式：重力仍在");
    }

    /* --- 切回 SENSOR：首个 tick 即可正常横移（状态已复位） --- */
    {
        uint8_t snap[8];
        int i, diff = 0;
        Tetris_SetInputMode(TETRIS_INPUT_IR);
        Tetris_Reset();
        set_board((const uint8_t[8]){0,0,0,0,0,0,0,0});
        tick_n(-40.0f, 0, 0, 1.0f, 3);          /* IR 模式：不动作 */
        memcpy(snap, Tetris_GetFrame(), 8);
        Tetris_SetInputMode(TETRIS_INPUT_SENSOR);
        tick_n(-40.0f, 0, 0, 1.0f, 1);          /* 切回后第一个 tick */
        for (i = 0; i < 8; i++) diff |= (Tetris_GetFrame()[i] != snap[i]);
        CHECK(diff, "切回 SENSOR 后立刻可横移");
        Tetris_SetInputMode(TETRIS_INPUT_SENSOR);
    }
```

- [ ] **Step 2: 运行测试确认失败**

Run:
```bash
cd /d/claude/hw-projects/stm32f103-mpu6050-oled
export PATH="/c/msys64/mingw64/bin:$PATH"
gcc -std=c99 -Wall -Wextra -IUser -IUser/tetris -o build/test_tetris.exe tests/test_tetris.c User/tetris/tetris.c -lm && ./build/test_tetris.exe
```
Expected: 链接失败，`undefined reference to Tetris_HardDrop` / `Tetris_SetInputMode` / `Tetris_SoftDrop`。

- [ ] **Step 3: 改 `User/tetris/tetris.h`**

在 `Tetris_Rotate(void);` 声明之后插入：

```c
/**
 * @brief  手动软降：下移一格；撞底则锁定并出下一块
 * @retval 1=下移成功 0=已锁定或游戏结束
 */
uint8_t Tetris_SoftDrop(void);

/**
 * @brief  硬降：一路落到碰撞前，锁定并出下一块（不计分）
 * @retval 掉落格数（游戏结束时返回 0 且不动作）
 */
uint8_t Tetris_HardDrop(void);

/* 输入模式：SENSOR = 体感（倾斜/敲击/roll 软降），IR = 遥控器全接管 */
typedef enum {
    TETRIS_INPUT_SENSOR = 0,
    TETRIS_INPUT_IR = 1
} TetrisInputMode;

/**
 * @brief  切换输入源；两个方向都复位体感状态（已武装/未按下/无冷却）
 */
void Tetris_SetInputMode(TetrisInputMode mode);
TetrisInputMode Tetris_GetInputMode(void);
```

- [ ] **Step 4: 改 `User/tetris/tetris.c`**

(a) 在体感状态变量块（`static uint16_t s_tap_mute_ms;` 之后）加：

```c
static TetrisInputMode s_input_mode;   /* BSS 零值 = TETRIS_INPUT_SENSOR */
```

(b) 在 `Tetris_Rotate()` 之后加三个新函数：

```c
uint8_t Tetris_SoftDrop(void)
{
    if (s_game_over) return 0;
    if (!collide(&s_cur, s_cur.x, s_cur.y + 1))
    {
        s_cur.y++;
        return 1;
    }
    lock_piece();
    return 0;
}

uint8_t Tetris_HardDrop(void)
{
    uint8_t rows = 0;
    if (s_game_over) return 0;
    while (!collide(&s_cur, s_cur.x, s_cur.y + 1))
    {
        s_cur.y++;
        rows++;
    }
    lock_piece();
    return rows;
}

void Tetris_SetInputMode(TetrisInputMode mode)
{
    s_input_mode = mode;
    /* 两个方向都回到"已武装 / 未按下 / 无冷却"，切换后从干净状态起步 */
    s_tilt_armed = 1;
    s_tap_pressed = 0;
    s_tap_cooldown_ms = 0;
}

TetrisInputMode Tetris_GetInputMode(void)
{
    return s_input_mode;
}
```

(c) 整段替换 `Tetris_Tick()`（新增 SENSOR 模式门控；冷却/重力计时留在门控外）：

```c
void Tetris_Tick(float tilt_deg, float roll_deg, float ax, float ay, float az)
{
    float tilt = TETRIS_TILT_INVERT ? -tilt_deg : tilt_deg;

    if (s_tap_cooldown_ms) s_tap_cooldown_ms -= 10;
    if (s_tap_mute_ms >= 10) s_tap_mute_ms -= 10; else s_tap_mute_ms = 0;
    if (s_drop_acc_ms < 0xFFFFFFFFu) s_drop_acc_ms += 10;

    if (s_game_over)
    {
        /* 堆到顶：点阵全亮保持 TETRIS_OVER_HOLD_MS，倒计时结束后自动重开 */
        if (s_over_hold_ms >= 10U)
        {
            s_over_hold_ms -= 10U;
        }
        else
        {
            Tetris_Reset();
        }
        return;
    }

    /* 体感判定只在 SENSOR 模式参与；IR 模式仅保留重力与结束计时（遥控器全接管） */
    if (s_input_mode == TETRIS_INPUT_SENSOR)
    {
        float norm = sqrtf(ax * ax + ay * ay + az * az);

        /* --- 倾斜侧移：边沿触发，回中（阈值-死区以内）再武装 --- */
        if (s_tilt_armed && tilt >= TETRIS_TILT_THRESHOLD)
        {
            Tetris_ShiftRight();
            s_tilt_armed = 0;
        }
        else if (s_tilt_armed && tilt <= -TETRIS_TILT_THRESHOLD)
        {
            Tetris_ShiftLeft();
            s_tilt_armed = 0;
        }
        else if (!s_tilt_armed && tilt < TETRIS_TILT_THRESHOLD - TETRIS_TILT_DEADBAND
                                && tilt > -(TETRIS_TILT_THRESHOLD - TETRIS_TILT_DEADBAND))
        {
            s_tilt_armed = 1;
        }

        /* --- 敲击旋转：尖峰触发，回落武装 + 冷却双闸；按键震动期间静音 --- */
        if (!s_tap_pressed && !s_tap_mute_ms && norm >= TETRIS_TAP_PEAK_G && !s_tap_cooldown_ms)
        {
            s_tap_pressed = 1;
            Tetris_Rotate();
            s_tap_cooldown_ms = TETRIS_TAP_COOLDOWN_MS;
        }
        if (s_tap_pressed && norm < TETRIS_TAP_RELEASE_G)
        {
            s_tap_pressed = 0;
        }
    }

    /* --- 重力下落：周期随消行数缩短（每 5 行快 100ms，下限 TETRIS_DROP_MIN_MS）
     *     软降：仅 SENSOR 模式，roll 超过 ±20° 时 60ms/格 --- */
    {
        uint32_t period = TETRIS_DROP_BASE_MS - (s_lines / 5) * 100U;
        if (period < TETRIS_DROP_MIN_MS) period = TETRIS_DROP_MIN_MS;
        if (s_input_mode == TETRIS_INPUT_SENSOR &&
            (roll_deg >= TETRIS_SOFTDROP_ROLL_DEG || roll_deg <= -TETRIS_SOFTDROP_ROLL_DEG))
            period = TETRIS_DROP_SOFT_MS;
        if (s_drop_acc_ms >= period)
        {
            s_drop_acc_ms = 0;
            if (!collide(&s_cur, s_cur.x, s_cur.y + 1))
                s_cur.y++;
            else
                lock_piece();
        }
    }
}
```

- [ ] **Step 5: 运行测试确认全绿**

Run:
```bash
cd /d/claude/hw-projects/stm32f103-mpu6050-oled
export PATH="/c/msys64/mingw64/bin:$PATH"
gcc -std=c99 -Wall -Wextra -IUser -IUser/tetris -o build/test_tetris.exe tests/test_tetris.c User/tetris/tetris.c -lm && ./build/test_tetris.exe
```
Expected: 全绿（含原有 14 组），零警告。

- [ ] **Step 6: 提交**

```bash
cd /d/claude/hw-projects/stm32f103-mpu6050-oled
git add User/tetris/tetris.h User/tetris/tetris.c tests/test_tetris.c
git commit -m "feat(tetris): 硬降/软降 API + IR/SENSOR 输入模式（体感判定可整体屏蔽）"
```

---

### Task 3: PB6 EXTI 接收层与构建接入

**Files:**
- Create: `User/ir/ir_rx.h`
- Create: `User/ir/ir_rx.c`
- Modify: `User/stm32f1xx_it.c`
- Modify: `Makefile`

**Interfaces:**
- Consumes: Task 1 的 `IrDecode_Reset()` / `IrDecode_Edge()` / `IrFrame_t` / `IR_FRAME_DATA` / `IR_FRAME_REPEAT`
- Produces:
  - `typedef struct { uint8_t kind; uint8_t addr; uint8_t cmd; uint8_t valid; uint8_t standard; uint32_t raw; } IrEvent_t;`
  - `#define IR_EV_DATA 1` / `#define IR_EV_REPEAT 2`
  - `void IrRx_Init(void);` / `uint8_t IrRx_Poll(IrEvent_t *ev);`

- [ ] **Step 1: 写 `User/ir/ir_rx.h`**

```c
/**
  ******************************************************************************
  * @file    ir_rx.h
  * @brief   红外接收层：PB6 双边沿 EXTI + DWT 时间戳 + 事件队列
  *
  *  ISR 只做三件事：读 CYCCNT 算间隔 → 喂纯解码器 → 完整帧入队。
  *  动作派发一律留给主循环 IrRx_Poll()。
  ******************************************************************************
  */
#ifndef __IR_RX_H
#define __IR_RX_H

#include <stdint.h>

#define IR_EV_DATA    1
#define IR_EV_REPEAT  2

typedef struct {
    uint8_t  kind;      /* IR_EV_DATA / IR_EV_REPEAT */
    uint8_t  addr;
    uint8_t  cmd;
    uint8_t  valid;     /* 1 = 命令补码校验通过（动作门槛） */
    uint8_t  standard;  /* 1 = 地址补码也成立（扩展 NEC 时为 0） */
    uint32_t raw;       /* 原始 32 位，供 RTT 扫码/排障打印 */
} IrEvent_t;

void    IrRx_Init(void);
uint8_t IrRx_Poll(IrEvent_t *ev);

#endif /* __IR_RX_H */
```

- [ ] **Step 2: 写 `User/ir/ir_rx.c`**

```c
/**
  ******************************************************************************
  * @file    ir_rx.c
  * @brief   PB6(EXTI6) 双边沿接收 + DWT 微秒时间戳
  *
  * @优先级   NVIC 1（SysTick 为 0）：毫秒计时与 HAL_Delay 不受影响
  * @回绕     CYCCNT 32 位 @72MHz 每 59.6s 回绕，全部用无符号差值
  * @队列     4 深度；主循环每轮取一次，OLED 整帧阻塞 13ms 也不会溢出
  ******************************************************************************
  */
#include "ir_rx.h"
#include "ir_decode.h"
#include "stm32f1xx_hal.h"

#define IR_PORT        GPIOB
#define IR_PIN         GPIO_PIN_6
#define IR_IRQn        EXTI9_5_IRQn
#define IR_NVIC_PRIO   1U
#define IR_Q_DEPTH     4U

static volatile IrEvent_t s_q[IR_Q_DEPTH];
static volatile uint8_t   s_head, s_tail;
static uint32_t           s_last_cyc;

void IrRx_Init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_AFIO_CLK_ENABLE();

    /* 接收头空闲输出高、收到 38kHz 载波时拉低；双边沿都要 */
    gpio.Pin  = IR_PIN;
    gpio.Mode = GPIO_MODE_IT_RISING_FALLING;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(IR_PORT, &gpio);

    HAL_NVIC_SetPriority(IR_IRQn, IR_NVIC_PRIO, 0);
    HAL_NVIC_EnableIRQ(IR_IRQn);

    IrDecode_Reset();
    s_head = 0;
    s_tail = 0;
    s_last_cyc = DWT->CYCCNT;
}

uint8_t IrRx_Poll(IrEvent_t *ev)
{
    if (s_head == s_tail) return 0;
    *ev = s_q[s_tail];
    s_tail = (uint8_t)((s_tail + 1U) % IR_Q_DEPTH);
    return 1;
}

/* HAL_GPIO_EXTI_IRQHandler 会清挂起位后回调这里 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    IrFrame_t f;
    uint32_t now, dt_us;
    uint8_t  level, next;

    if (GPIO_Pin != IR_PIN) return;

    now    = DWT->CYCCNT;
    dt_us  = (now - s_last_cyc) / (SystemCoreClock / 1000000U);
    s_last_cyc = now;
    level  = (HAL_GPIO_ReadPin(IR_PORT, IR_PIN) == GPIO_PIN_SET) ? 1U : 0U;

    if (!IrDecode_Edge(level, dt_us, &f)) return;

    next = (uint8_t)((s_head + 1U) % IR_Q_DEPTH);
    if (next == s_tail) return;          /* 队列满：丢新帧 */
    s_q[s_head].kind     = (f.kind == IR_FRAME_REPEAT) ? IR_EV_REPEAT : IR_EV_DATA;
    s_q[s_head].addr     = f.addr;
    s_q[s_head].cmd      = f.cmd;
    s_q[s_head].valid    = f.valid;
    s_q[s_head].standard = f.standard;
    s_q[s_head].raw      = f.raw;
    s_head = next;
}
```

- [ ] **Step 3: 在 `User/stm32f1xx_it.c` 加中断入口**

在 `SysTick_Handler` 之后、HAL 桩注释之前插入：

```c
/**
  * @brief  EXTI9_5 中断：红外接收（PB6）
  */
void EXTI9_5_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_6);
}
```

- [ ] **Step 4: 改 `Makefile`（源文件 + include）**

在 `C_SOURCES` 的 `User/tetris/tetris.c \` 之后加两行：

```makefile
User/ir/ir_decode.c \
User/ir/ir_rx.c \
```

在 `C_INCLUDES` 的 `-IUser/tetris \` 之后加一行：

```makefile
-IUser/ir \
```

在 `test:` 目标的 `test_tetris.exe` 两行之后加：

```makefile
	$(HOST_ENV) $(HOST_CC) -pipe -std=c99 -Wall -Wextra -IUser -IUser/ir -o $(BUILD_DIR)/test_ir_decode.exe tests/test_ir_decode.c User/ir/ir_decode.c
	$(BUILD_DIR)/test_ir_decode.exe
```

- [ ] **Step 5: 构建确认零警告 + 尺寸在预算内**

Run:
```bash
cd /d/claude/hw-projects/stm32f103-mpu6050-oled
make clean && make 2>&1 | tail -5
arm-none-eabi-size build/mpu6050-oled.elf
```
Expected: 构建成功、无 `warning:`；`text` 比基线 37432 增长 ≤ 4096，`bss+data` 比基线 4096 增长 ≤ 64。

- [ ] **Step 6: 跑 host 测试确认 `make test` 全绿**

Run:
```bash
cd /d/claude/hw-projects/stm32f103-mpu6050-oled
make test
```
Expected: 三个测试程序全部输出 `== 结果: N/N 通过 ==`。

- [ ] **Step 7: 提交**

```bash
cd /d/claude/hw-projects/stm32f103-mpu6050-oled
git add User/ir/ir_rx.h User/ir/ir_rx.c User/stm32f1xx_it.c Makefile
git commit -m "feat(ir): PB6 EXTI 接收层 + DWT 微秒时间戳 + 事件队列，接入构建"
```

---

### Task 4: 键映射与主循环派发

**Files:**
- Create: `User/ir/ir_keymap.h`
- Modify: `User/main.c`
- Modify: `User/oled_view/oled_view.c`

**Interfaces:**
- Consumes: Task 2 的 `Tetris_HardDrop` / `Tetris_SoftDrop` / `Tetris_SetInputMode` / `Tetris_GetInputMode` / `TETRIS_INPUT_IR`；Task 3 的 `IrRx_Init` / `IrRx_Poll` / `IrEvent_t` / `IR_EV_DATA` / `IR_EV_REPEAT`
- Produces: `IR_KEY_LEFT` / `IR_KEY_RIGHT` / `IR_KEY_DOWN` / `IR_KEY_ROTATE` / `IR_KEY_HARDDROP` / `IR_KEY_MODE` 六个宏

- [ ] **Step 1: 写 `User/ir/ir_keymap.h`**

```c
/**
  ******************************************************************************
  * @file    ir_keymap.h
  * @brief   NEC 命令字节 → 功能映射（只匹配 cmd，不匹配 addr）
  *
  *  ⚠️ 默认值 = 常见 21 键 NEC 迷你遥控，未经实测。
  *  扫码校准：烧录后按一遍遥控器，从 RTT 的 [IR] addr=.. cmd=.. 行读值，
  *  只改本文件重编即完成适配，逻辑代码不动。
  ******************************************************************************
  */
#ifndef __IR_KEYMAP_H
#define __IR_KEYMAP_H

#define IR_KEY_LEFT      0x44U
#define IR_KEY_RIGHT     0x43U
#define IR_KEY_DOWN      0x07U
#define IR_KEY_ROTATE    0x15U
#define IR_KEY_HARDDROP  0x40U
#define IR_KEY_MODE      0x09U

#endif /* __IR_KEYMAP_H */
```

- [ ] **Step 2: 改 `User/main.c`（include + 派发函数）**

在 include 区 `#include "tetris.h"` 之后加：

```c
#include "ir_rx.h"
#include "ir_keymap.h"
```

在 `static uint32_t s_last_rtt_tick = 0;` 之后加：

```c
/* 控制模式：0=SENSOR(MPU 体感) 1=IR(遥控器全接管)，上电默认体感 */
static uint8_t s_ctrl_mode = TETRIS_INPUT_SENSOR;
```

在 `int main(void)` 之前加派发函数：

```c
/**
  * @brief  红外事件派发（主循环调用；ISR 只入队，不做动作）
  * @note   MODE 键两种模式都生效；其余动作键仅 IR 模式生效；
  *         重复帧只驱动左/右/软降，旋转与硬降忽略重复帧
  */
static void ir_dispatch(void)
{
    IrEvent_t ev;
    char buf[80];

    while (IrRx_Poll(&ev))
    {
        if (ev.kind == IR_EV_DATA)
        {
            snprintf(buf, sizeof(buf), "[IR] addr=0x%02X cmd=0x%02X raw=0x%08lX%s\n",
                     (unsigned)ev.addr, (unsigned)ev.cmd, (unsigned long)ev.raw,
                     ev.valid ? "" : " BAD");
            SEGGER_RTT_WriteString(0, buf);
        }
        if (!ev.valid) continue;                 /* 命令补码不过 → 不动作 */

        if (ev.cmd == IR_KEY_MODE)
        {
            if (ev.kind == IR_EV_REPEAT) continue;
            s_ctrl_mode = (s_ctrl_mode == TETRIS_INPUT_SENSOR)
                          ? (uint8_t)TETRIS_INPUT_IR : (uint8_t)TETRIS_INPUT_SENSOR;
            Tetris_SetInputMode((TetrisInputMode)s_ctrl_mode);
            SEGGER_RTT_WriteString(0, s_ctrl_mode ? "[IR] mode=IR\n" : "[IR] mode=MPU\n");
            continue;
        }

        if (s_ctrl_mode != TETRIS_INPUT_IR) continue;   /* MPU 模式下遥控动作键不生效 */

        switch (ev.cmd)
        {
        case IR_KEY_LEFT:
            Tetris_ShiftLeft();
            break;
        case IR_KEY_RIGHT:
            Tetris_ShiftRight();
            break;
        case IR_KEY_DOWN:
            Tetris_SoftDrop();
            break;
        case IR_KEY_ROTATE:
            if (ev.kind == IR_EV_REPEAT) break;
            Tetris_Rotate();
            SEGGER_RTT_WriteString(0, "[IR] rotate\n");
            break;
        case IR_KEY_HARDDROP:
            if (ev.kind == IR_EV_REPEAT) break;
            {
                uint8_t rows = Tetris_HardDrop();
                snprintf(buf, sizeof(buf), "[IR] harddrop rows=%u\n", (unsigned)rows);
                SEGGER_RTT_WriteString(0, buf);
            }
            break;
        default:
            break;
        }
    }
}
```

- [ ] **Step 3: 在 `main()` 初始化段接入**

在 `Key_Init();` 之后加：

```c
    IrRx_Init();
    SEGGER_RTT_WriteString(0, "[IR] init pin=PB6\n");
    SEGGER_RTT_WriteString(0, "[IR] mode=MPU\n");
```

在 `while (1)` 主循环内、`Key_Tick();` 之后加：

```c
        /* 红外遥控事件派发（ISR 只入队，动作在这里做） */
        ir_dispatch();
```

- [ ] **Step 4: 改 `User/oled_view/oled_view.c` 页 3 底部提示行**

原代码：

```c
    if (Tetris_IsGameOver())
        My_OLED_ShowString(0, 56, (char *)"NEW GAME...", 6);
    else
        My_OLED_ShowString(0, 56, (char *)"TILT:MOVE PB15:ROT", 6);
```

改成：

```c
    if (Tetris_IsGameOver())
        My_OLED_ShowString(0, 56, (char *)"NEW GAME...", 6);
    else if (Tetris_GetInputMode() == TETRIS_INPUT_IR)
        My_OLED_ShowString(0, 56, (char *)"CTRL:IR  REMOTE", 6);
    else
        My_OLED_ShowString(0, 56, (char *)"CTRL:MPU TILT+TAP", 6);
```

- [ ] **Step 5: 构建 + 测试**

Run:
```bash
cd /d/claude/hw-projects/stm32f103-mpu6050-oled
make clean && make 2>&1 | tail -5 && make test
```
Expected: 零警告；三个 host 测试全绿；`arm-none-eabi-size build/mpu6050-oled.elf` 仍在预算内。

- [ ] **Step 6: 提交**

```bash
cd /d/claude/hw-projects/stm32f103-mpu6050-oled
git add User/ir/ir_keymap.h User/main.c User/oled_view/oled_view.c
git commit -m "feat(ir): 遥控键位映射 + 主循环派发 + OLED 页3 模式指示"
```

---

### Task 5: 规格/文档/验收基线

**Files:**
- Modify: `.workbench/expectations.json`
- Modify: `docs/FSD.md`
- Create: `docs/ADR/005-ir-exti-dwt-decode.md`
- Modify: `README.md`

**Interfaces:**
- Consumes: Task 1~4 的全部产出（日志格式 `[IR] init pin=PB6` / `[IR] addr=0x.. cmd=0x.. raw=0x........` / `[IR] mode=IR|MPU` / `[IR] rotate` / `[IR] harddrop rows=N`）
- Produces: 机器可跑的 FR-IR 断言 + 人工核对清单

- [ ] **Step 1: `.workbench/expectations.json` 追加 4 条机器断言**

在 `"expectations": [` 数组末尾（最后一个对象之后）追加：

```json
    {
      "id": "FR-IR-01",
      "desc": "红外接收初始化，启动日志标明引脚",
      "texts": [
        "[IR] init pin=PB6"
      ]
    },
    {
      "id": "FR-IR-02",
      "desc": "每个 NEC 帧打印地址/命令/原始值",
      "patterns": [
        "\\[IR\\] addr=0x[0-9A-F]{2} cmd=0x[0-9A-F]{2} raw=0x[0-9A-F]{8}"
      ]
    },
    {
      "id": "FR-IR-03",
      "desc": "MODE 键双向切换（采集窗内按两次）",
      "texts": [
        "[IR] mode=IR",
        "[IR] mode=MPU"
      ]
    },
    {
      "id": "FR-IR-04",
      "desc": "IR 模式下旋转与硬降动作（采集窗内各按一次）",
      "texts": [
        "[IR] rotate",
        "[IR] harddrop rows="
      ]
    }
```

**FR-IR-05 / FR-IR-06 / FR-IR-07 不进机器断言**：它们是"某段日志**不**出现"的负向判定，与 FR-IR-02/04 的正向断言在同一采集窗里互相冲突，改为 README 人工核对项（见 Step 4）。

- [ ] **Step 2: `docs/FSD.md` 追加功能需求**

在文件末尾追加：

```markdown
## 7. 红外遥控需求（v0.11，2026-11-01）

### FR-IR-01：红外接收初始化
上电调用 `IrRx_Init()`（PB6 / EXTI9_5 / NVIC 优先级 1），启动日志输出 `[IR] init pin=PB6`。

### FR-IR-02：NEC 帧解码与 `~` 校验
每收到一个帧输出 `[IR] addr=0xAA cmd=0xCC raw=0xXXXXXXXX`；命令补码不成立时追加 ` BAD` 且不产生动作。取地址只作日志与扩展 NEC 标记，不作动作门槛。

### FR-IR-03：双控制模式
上电默认 MPU（SENSOR）模式并输出 `[IR] mode=MPU`。MODE 键在两种模式下都生效，切换时输出 `[IR] mode=IR` / `[IR] mode=MPU`。

### FR-IR-04：IR 模式动作
IR 模式下：左/右移、软降、旋转、硬降由遥控器驱动；旋转输出 `[IR] rotate`，硬降输出 `[IR] harddrop rows=N`。

### FR-IR-05：IR 模式下体感失效
倾斜、敲击、roll 软降三项判定不参与，重力下落与结束后自动重开照常。

### FR-IR-06：MPU 模式下遥控动作键不生效
MPU 模式按旋转/硬降/位移键不产生任何游戏动作，也不打印 `[IR] rotate` / `[IR] harddrop`。

### FR-IR-07：重复帧只驱动位移与软降
按住遥控器产生的重复帧只作用于左/右/软降，旋转/硬降/MODE 忽略重复帧。

### NFR-05：中断纪律与资源
ISR 内禁止 `SEGGER_RTT_*`、`snprintf`、`HAL_Delay`；flash 增量 ≤ 4KB；RAM 增量 ≤ 96B。
```

- [ ] **Step 3: 新建 `docs/ADR/005-ir-exti-dwt-decode.md`**

```markdown
# ADR-005：红外接收用 EXTI + DWT 时间戳，不用定时器捕获、不用轮询

- 状态：已采纳
- 日期：2026-11-01
- 关联：FR-IR-01~07

## 背景

0038 一体化接收头输出 38kHz 解调后的 NEC 波形：前导 9ms 低 + 4.5ms 高，位宽 560µs / 1690µs，
最短判定单元 560µs。需要把边沿间隔量出来再判位。

## 决策

PB6 配 `GPIO_MODE_IT_RISING_FALLING`，ISR 只读 `DWT->CYCCNT`（72MHz，13.9ns）算微秒间隔，
喂给纯 C 状态机 `IrDecode_Edge()`；解出完整帧后压入 4 深度环形队列，主循环 `IrRx_Poll()` 取。

## 否决项

1. **主循环轮询**：OLED 软件 I2C 整帧刷屏阻塞最坏 ~13ms，而一个 `0` 位只有 1.12ms，必然漏码。
2. **TIM 输入捕获**：硬件捕获精度（ns 级）对 560µs 位宽是过剩；代价是要启用 `HAL_TIM_MODULE`、
   Makefile 加入 `hal_tim.c` / `hal_tim_ex.c`（flash +3~5KB）、处理 CCR 溢出，且多一套 HAL 句柄。
   EXTI 中断延迟抖动 ~1µs 对位宽判定误差 0.2%，完全可接受。

## 后果

- DWT 必须在 `IrRx_Init()` 之前使能（现由 `SystemClock_72MHz()` 完成）。
- CYCCNT 32 位 @72MHz 每 59.6s 回绕，全部用无符号差值处理。
- 升级到需要更短码元（如 RC5 的 889µs 半位）的协议时，需重新评估捕获路径。
```

- [ ] **Step 4: `README.md` 更新**

(a) 「硬件接线」表格末尾加三行：

```markdown
| IR OUT | **PB6** | 0038 一体化接收头输出，EXTI 双边沿（空闲高） |
| IR VCC | **3.3V** | 接收头 2.7–5.5V 可用；用 3.3V 免电平转换 |
| IR GND | **GND** | — |
```

(b) 接线说明段后加：

```markdown
> 红外接收头供电建议按 datasheet 串 100Ω + 对地并 100µF；OUT 已开 MCU 内部上拉，长线（>15cm）再并 10k。
> 0038 三脚定义（OUT/GND/VCC）各厂不一致，**以实物丝印为准**；接反不坏，静态量 OUT 应为 ≈3.3V。
```

(c) 新增「红外遥控（v0.11）」小节：

```markdown
## 红外遥控（v0.11）

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
```

(d) 「真机验证要人工按键」小节的人工清单追加：

```markdown
### 红外相关人工核对（v0.11 新增，不进 expectations.json）

| 项 | 动作 | 通过判据 |
|---|---|---|
| FR-IR-05 | IR 模式下倾斜板子 | 方块不横移；重力仍在下落 |
| FR-IR-06 | 切回 MPU 模式后按遥控旋转键 | 无 `[IR] rotate`，方块不转 |
| FR-IR-07 | IR 模式下按住旋转键 2s | 只出现 1 行 `[IR] rotate` |
```

(e) 「文档」列表加：

```markdown
- [ADR-005](docs/ADR/005-ir-exti-dwt-decode.md) - 红外接收用 EXTI + DWT 时间戳
```

- [ ] **Step 5: 提交文档**

```bash
cd /d/claude/hw-projects/stm32f103-mpu6050-oled
git add .workbench/expectations.json docs/FSD.md docs/ADR/005-ir-exti-dwt-decode.md README.md
git commit -m "docs(v0.11): FR-IR-01~07 + NFR-05、ADR-005、README 接线与键值校准流程"
```

- [ ] **Step 6: 真机验收（需用户在场接线并按遥控器）**

先跑硬件预检，再接板子：

```bash
python D:/claude/tools/cc-ops/hw_preflight.py
cd /d/claude/hw-projects/stm32f103-mpu6050-oled
make flash
```

采集窗内人工时序（顺序不可变）：

1. 上电静置 2s（FR-IR-01 / FR-BOOT-01/02 / FR-MPU-01）
2. 遥控器按「旋转键」一次，读到 `[IR] rotate` → 若码值不是 0x15，记下来做 Step 7 校准
3. 遥控器按 MODE 键一次 → `[IR] mode=IR`
4. 遥控器按右移、软降、硬降各一次 → `[IR] harddrop rows=N`
5. 倾斜板子，确认方块不横移（FR-IR-05 人工）
6. 遥控器再按 MODE → `[IR] mode=MPU`
7. 按遥控旋转键，确认无 `[IR] rotate`（FR-IR-06 人工）
8. PB15 长按（`[CAL] done`）→ PA0 短按 ×3（`[STATE=3`）→ PA0 长按关屏（**最后**）

然后：

```bash
python D:/claude/embedded-toolkit/scripts/verify.py --project .
```

Expected: 原 13 项 + FR-IR-01~04 全 PASS（人工项按上表勾）。

- [ ] **Step 7: 按实测码值校准 `ir_keymap.h`（仅当 Step 6 码值不符时）**

把 Step 6 读到 6 个 cmd 值填进 `User/ir/ir_keymap.h`，`make` 后重跑 Step 6 的 2~4 步确认动作正确，提交：

```bash
cd /d/claude/hw-projects/stm32f103-mpu6050-oled
git add User/ir/ir_keymap.h
git commit -m "fix(ir): 按实测遥控器码值校准键位映射"
```

---

## 验收标准（全部满足才算完成）

1. `make` 零警告；flash 增量 ≤ 4KB。
2. `make test` 三个 host 测试程序全绿（含新增 `test_ir_decode` 12 组、`test_tetris` 新增 4 组）。
3. `verify.py`：原 13 项不回归 + FR-IR-01~04 PASS。
4. README 人工清单 FR-IR-05/06/07 三项人工确认为通过。
5. 上电默认 MPU 模式，行为与 v0.10 一致（体感三件套可用）。
6. IR 模式下遥控器可完整玩一局：左右移、软降、旋转、硬降、MODE 切回。
