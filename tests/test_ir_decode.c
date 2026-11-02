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
