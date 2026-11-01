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
