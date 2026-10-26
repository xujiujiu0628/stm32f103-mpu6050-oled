/**
  ******************************************************************************
  * @file    tetris.c
  * @brief   8x8 俄罗斯方块核心逻辑（纯 C，无硬件依赖，host 可测）
  *
  * @坐标系  board[0]=顶行 ... board[7]=底行；每字节一行的位图，bit7=最左列
  * @控制    roll 倾斜左右移（边沿触发+回中再武装）；|a| 尖峰旋转（回落+冷却双闸）
  * @结束    出块位置被占 → GAME OVER；敲击重开
  ******************************************************************************
  */
#include "tetris.h"
#include <math.h>
#include <stdlib.h>

/* ---- 7 种方块，4 个旋态，各为 4x4 位图（bit15 = 盒内(0,0)左上，行优先） ---- */
/* 逐行 4bit：val = (r0<<12)|(r1<<8)|(r2<<4)|r3 */
static const uint16_t SHAPES[7][4] = {
    /* I */
    {0x0F00, 0x2222, 0x0F00, 0x2222},
    /* O */
    {0x6600, 0x6600, 0x6600, 0x6600},
    /* T */
    {0x0E40, 0x0644, 0x02E0, 0x4460},
    /* S */
    {0x06C0, 0x0462, 0x06C0, 0x0462},
    /* Z */
    {0x0C60, 0x0264, 0x0C60, 0x0264},
    /* J */
    {0x08E0, 0x0644, 0x0E20, 0x44C0},
    /* L */
    {0x02E0, 0x0446, 0x0E80, 0xC440},
};

typedef struct {
    uint8_t shape;   /* 0..6 */
    uint8_t rot;     /* 0..3 */
    int8_t  x;       /* 盒左上角列（可为负，盒 4 宽） */
    int8_t  y;       /* 盒左上角行（可为负，盒 4 高） */
} Piece_t;

static uint8_t  s_board[8];
static Piece_t  s_cur, s_next;
static uint16_t s_score, s_lines;
static uint8_t  s_game_over;
static uint32_t s_rng;                 /* LFSR 状态（取块） */

/* 体感状态机 */
static uint8_t  s_tilt_armed;          /* 倾斜触发武装 */
static uint8_t  s_tap_pressed;         /* 敲击按下态（|a| 尚未回落） */
static uint32_t s_tap_cooldown_ms;     /* 敲击冷却倒计时 */

/* 重力 */
static uint32_t s_drop_acc_ms;

/* ---- 内部工具 ---- */

static uint8_t next_random(void)
{
    /* 16 位 LFSR：异或反馈，出 0..6 */
    s_rng ^= s_rng << 7;
    s_rng ^= s_rng >> 9;
    s_rng ^= s_rng << 8;
    return (uint8_t)(s_rng % 7);
}

static void spawn_piece(Piece_t *p)
{
    p->shape = next_random();
    p->rot   = 0;
    p->x     = 2;    /* 4 宽盒居中于 8 列 */
    p->y     = 0;
}

/* 盒内位图第 (bx,by) 位 */
static int piece_cell(const Piece_t *p, int bx, int by)
{
    uint16_t m = SHAPES[p->shape][p->rot];
    return (m >> (15 - by * 4 - bx)) & 1;
}

/* 该块摆在 (x,y) 是否碰撞：出界（左右/底）或撞棋盘；顶上越界(y<0)不算 */
static int collide(const Piece_t *p, int x, int y)
{
    int bx, by;
    for (by = 0; by < 4; by++)
        for (bx = 0; bx < 4; bx++)
            if (piece_cell(p, bx, by))
            {
                int cx = x + bx, cy = y + by;
                if (cx < 0 || cx > 7 || cy > 7) return 1;
                if (cy >= 0 && (s_board[cy] >> (7 - cx)) & 1) return 1;
            }
    return 0;
}

static void lock_piece(void)
{
    int bx, by;
    for (by = 0; by < 4; by++)
        for (bx = 0; bx < 4; bx++)
            if (piece_cell(&s_cur, bx, by))
            {
                int cy = s_cur.y + by, cx = s_cur.x + bx;
                if (cy >= 0 && cy <= 7 && cx >= 0 && cx <= 7)
                    s_board[cy] |= (uint8_t)(0x80u >> cx);
            }

    /* 消行：满行 (0xFF) 抽掉，上方整体下坠，顶补空行 */
    {
        static const uint16_t line_score[5] = {0, 1, 3, 5, 8};
        uint8_t cleared = 0;
        int r, w;
        for (r = 7; r >= 0; r--)
        {
            if (s_board[r] == 0xFF) { cleared++; continue; }
            if (cleared)
            {
                w = r + cleared;
                if (w <= 7) s_board[w] = s_board[r];
            }
        }
        if (cleared)
        {
            uint8_t si = cleared > 4 ? 4 : cleared;   /* 8x8 棋盘可一次消 >4 行，查表封顶 */
            for (w = 0; w < cleared; w++) s_board[w] = 0x00;
            s_lines += cleared;
            s_score += line_score[si];
        }
    }

    /* 出下一块；出生位被占 → GAME OVER */
    s_cur = s_next;
    spawn_piece(&s_next);
    if (collide(&s_cur, s_cur.x, s_cur.y))
        s_game_over = 1;
}

/* ---- 对外 API ---- */

void Tetris_Reset(void)
{
    int i;
    for (i = 0; i < 8; i++) s_board[i] = 0;
    s_score = 0;
    s_lines = 0;
    s_game_over = 0;
    /* RNG 跨局持续演进：只在首次使用时给固定种子，重开不再回溯同一序列
     * （原来每次 Reset 写死 0xACE1 → 每局出块完全一样，2026-10-26 真机反馈）
     * 上电变异由 Tetris_Seed() 用传感器噪声注入 */
    if (!s_rng) s_rng = 0xACE1u;
    s_tilt_armed = 1;
    s_tap_pressed = 0;
    s_tap_cooldown_ms = 0;
    s_drop_acc_ms = 0;
    spawn_piece(&s_cur);
    spawn_piece(&s_next);
}

void Tetris_Seed(uint32_t seed)
{
    if (seed) s_rng ^= seed;
}

uint8_t Tetris_ShiftLeft(void)
{
    if (s_game_over) return 0;
    if (!collide(&s_cur, s_cur.x - 1, s_cur.y)) { s_cur.x--; return 1; }
    return 0;
}

uint8_t Tetris_ShiftRight(void)
{
    if (s_game_over) return 0;
    if (!collide(&s_cur, s_cur.x + 1, s_cur.y)) { s_cur.x++; return 1; }
    return 0;
}

void Tetris_Rotate(void)
{
    uint8_t old = s_cur.rot;
    if (s_game_over) return;
    s_cur.rot = (s_cur.rot + 1) & 3;
    /* 简单踢墙：旋转后碰撞则先试左右各挪一格，再不行回退 */
    if (collide(&s_cur, s_cur.x, s_cur.y))
    {
        if (!collide(&s_cur, s_cur.x - 1, s_cur.y))      s_cur.x--;
        else if (!collide(&s_cur, s_cur.x + 1, s_cur.y)) s_cur.x++;
        else s_cur.rot = old;
    }
}

const uint8_t *Tetris_GetFrame(void)
{
    static uint8_t frame[8];
    int bx, by, i;
    for (i = 0; i < 8; i++) frame[i] = s_board[i];
    if (!s_game_over)
        for (by = 0; by < 4; by++)
            for (bx = 0; bx < 4; bx++)
                if (piece_cell(&s_cur, bx, by))
                {
                    int cy = s_cur.y + by, cx = s_cur.x + bx;
                    if (cy >= 0 && cy <= 7 && cx >= 0 && cx <= 7)
                        frame[cy] |= (uint8_t)(0x80u >> cx);
                }
    return frame;
}

uint16_t Tetris_GetScore(void)     { return s_score; }
uint16_t Tetris_GetLines(void)     { return s_lines; }
uint8_t  Tetris_IsGameOver(void)   { return s_game_over; }
uint8_t  Tetris_GetCurrentShapeId(void) { return s_cur.shape; }
uint8_t  Tetris_GetNextShapeId(void)    { return s_next.shape; }

const uint8_t *Tetris_GetNextShape(void)
{
    /* 下一块渲染进 8x8 帧居中（4x4 盒放中间 4x4 区域） */
    static uint8_t f[8];
    int bx, by;
    for (bx = 0; bx < 8; bx++) f[bx] = 0;
    for (by = 0; by < 4; by++)
        for (bx = 0; bx < 4; bx++)
            if (piece_cell(&s_next, bx, by))
                f[2 + by] |= (uint8_t)(0x10u >> bx);   /* 行内 0x10=第2列起 */
    return f;
}

void Tetris_DebugSetBoard(const uint8_t rows[8])
{
    int i;
    for (i = 0; i < 8; i++) s_board[i] = rows[i];
}

void Tetris_Tick(float tilt_deg, float roll_deg, float ax, float ay, float az)
{
    float norm;
    float tilt = TETRIS_TILT_INVERT ? -tilt_deg : tilt_deg;

    if (s_tap_cooldown_ms) s_tap_cooldown_ms -= 10;
    if (s_drop_acc_ms < 0xFFFFFFFFu) s_drop_acc_ms += 10;

    norm = sqrtf(ax * ax + ay * ay + az * az);

    if (s_game_over)
    {
        /* 敲击重开：冷却已过 + 尖峰出现 + 回落后才武装，尖峰即重开 */
        if (!s_tap_pressed && norm >= TETRIS_TAP_PEAK_G && !s_tap_cooldown_ms)
        {
            s_tap_pressed = 1;
            Tetris_Reset();
        }
        if (s_tap_pressed && norm < TETRIS_TAP_RELEASE_G)
        {
            s_tap_pressed = 0;
            s_tap_cooldown_ms = TETRIS_TAP_COOLDOWN_MS;
        }
        return;
    }

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

    /* --- 敲击旋转：尖峰触发，回落武装 + 冷却双闸 --- */
    if (!s_tap_pressed && norm >= TETRIS_TAP_PEAK_G && !s_tap_cooldown_ms)
    {
        s_tap_pressed = 1;
        Tetris_Rotate();
        s_tap_cooldown_ms = TETRIS_TAP_COOLDOWN_MS;
    }
    if (s_tap_pressed && norm < TETRIS_TAP_RELEASE_G)
    {
        s_tap_pressed = 0;
    }

    /* --- 重力下落：周期随消行数缩短（每 5 行快 100ms，下限 TETRIS_DROP_MIN_MS）
     *     软降：roll 超过 ±20° 时 60ms/格（左右滚转板子 = 加速下落） --- */
    {
        uint32_t period = TETRIS_DROP_BASE_MS - (s_lines / 5) * 100U;
        if (period < TETRIS_DROP_MIN_MS) period = TETRIS_DROP_MIN_MS;
        if (roll_deg >= TETRIS_SOFTDROP_ROLL_DEG || roll_deg <= -TETRIS_SOFTDROP_ROLL_DEG)
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
