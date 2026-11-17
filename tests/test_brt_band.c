/**
  ******************************************************************************
  * @file    test_brt_band.c
  * @brief   亮度滞回纯函数 host 单测（哨兵法：撤掉滞回后本文件必 FAIL）
  *
  *  编译运行（Git Bash, 工程根）:
  *    /c/msys64/usr/bin/gcc -std=c99 -Wall -Wextra -User -o build/test_brt_band tests/test_brt_band.c User/brt_band.c
  *    ./build/test_brt_band
  *  方法源自 adc-oled My_OLED 哨兵单测教训: 判据完全由被测函数表达式定义,
  *  回归保护而非编译器巧合。
  ******************************************************************************
  */
#include <stdio.h>
#include "brt_band.h"

static int s_fail = 0;
static int s_total = 0;

#define CHECK(cond, name) do {                                   \
    s_total++;                                                   \
    if (!(cond)) { s_fail++; printf("  [FAIL] %s\n", name); }    \
} while (0)

#define EQ(a, b, name) CHECK((a) == (b), name)

int main(void)
{
    printf("== brt_band 滞回三档 ==\n");

    /* --- 越级直入（快拧）：任何 cur 下 HIGH 域无条件可达 --- */
    EQ(AdcBrt_NextBand(2800, BRT_BAND_LOW),  BRT_BAND_HIGH, "LOW快拧到HIGH直入");
    EQ(AdcBrt_NextBand(3000, BRT_BAND_MID),  BRT_BAND_HIGH, "MID直入HIGH");
    EQ(AdcBrt_NextBand(3300, BRT_BAND_HIGH), BRT_BAND_HIGH, "HIGH保持");

    /* --- LOW↔MID 滞回双沿 --- */
    EQ(AdcBrt_NextBand(1149, BRT_BAND_LOW),  BRT_BAND_LOW,  "LOW: 1149 未达上沿不动");
    EQ(AdcBrt_NextBand(1150, BRT_BAND_LOW),  BRT_BAND_MID,  "LOW: 1150 过上沿进 MID");
    EQ(AdcBrt_NextBand(1050, BRT_BAND_MID),  BRT_BAND_MID,  "MID: 1050 未破下沿不动  <-- 撤滞回则 FAIL");
    EQ(AdcBrt_NextBand(1000, BRT_BAND_MID),  BRT_BAND_LOW,  "MID: 1000 破下沿出 LOW");

    /* --- MID↔HIGH 滞回双沿 --- */
    EQ(AdcBrt_NextBand(2649, BRT_BAND_MID),  BRT_BAND_MID,  "MID: 2649 未达上沿不动");
    EQ(AdcBrt_NextBand(2650, BRT_BAND_MID),  BRT_BAND_HIGH, "MID: 2650 过上沿进 HIGH");
    EQ(AdcBrt_NextBand(2750, BRT_BAND_HIGH), BRT_BAND_HIGH, "HIGH: 2750 未破下沿不动 <-- 撤滞回则 FAIL");
    EQ(AdcBrt_NextBand(2649, BRT_BAND_HIGH), BRT_BAND_MID,  "HIGH: 2649 破下沿出 MID");

    /* --- 边界附近抖动序列（电位器噪声场景）: 1100/1120/1130 反复不得 LOW↔MID 振 --- */
    {
        Brt_Band_t b = BRT_BAND_LOW;
        int flips = 0, i;
        uint16_t noisy[] = {1100, 1120, 1130, 1110, 1125, 1100, 1115};
        Brt_Band_t prev = b;
        for (i = 0; i < 7; i++)
        {
            b = AdcBrt_NextBand(noisy[i], b);
            if (b != prev) { flips++; prev = b; }
        }
        EQ(flips, 0, "噪声带内(1100-1130)零翻转");
    }
    /* 越过上沿后回落带内: 恰好一次翻转再不振 */
    {
        Brt_Band_t b = BRT_BAND_LOW;
        b = AdcBrt_NextBand(1200, b);              /* 进 MID */
        b = AdcBrt_NextBand(1120, b);              /* 带内回落 */
        EQ(b, BRT_BAND_MID, "越过上沿后带内回弹留 MID（单次翻转）");
    }
    /* LOW 域内小幅抖动零翻转 */
    {
        Brt_Band_t b = BRT_BAND_LOW;
        b = AdcBrt_NextBand(990, b);
        b = AdcBrt_NextBand(1010, b);              /* 1000 下沿附近的抖动 */
        b = AdcBrt_NextBand(985, b);
        EQ(b, BRT_BAND_LOW, "LOW 域 1000 附边抖动不翻");   /* 注: 990→LOW,1010 在 LOW 态未达 1150 仍 LOW */
    }

    /* --- 极端值与非法态 --- */
    EQ(AdcBrt_NextBand(0, BRT_BAND_HIGH), BRT_BAND_MID, "0mV 从 HIGH 落 MID（越级回落一档）");
    EQ(AdcBrt_NextBand(0, BRT_BAND_MID),  BRT_BAND_LOW, "0mV 从 MID 落 LOW");
    EQ(AdcBrt_NextBand(0, BRT_BAND_LOW),  BRT_BAND_LOW, "0mV LOW 保持");
    EQ(AdcBrt_NextBand(1500, (Brt_Band_t)99), BRT_BAND_MID, "非法 cur 兜底 MID");

    /* --- 查表 --- */
    EQ(AdcBrt_ContrastOf(BRT_BAND_LOW),  BRT_CONTRAST_LOW,  "对比度 LOW=0x40");
    EQ(AdcBrt_ContrastOf(BRT_BAND_MID),  BRT_CONTRAST_MID,  "对比度 MID=0x8E");
    EQ(AdcBrt_ContrastOf(BRT_BAND_HIGH), BRT_CONTRAST_HIGH, "对比度 HIGH=0xFF");

    /* --- 点阵亮度查表（2026-11-17: 同一只电位器同时驱动 OLED 与点阵） --- */
    EQ(AdcBrt_MatrixIntensityOf(BRT_BAND_LOW),  BRT_MATRIX_LOW,  "点阵亮度 LOW=0x02");
    EQ(AdcBrt_MatrixIntensityOf(BRT_BAND_MID),  BRT_MATRIX_MID,  "点阵亮度 MID=0x07");
    EQ(AdcBrt_MatrixIntensityOf(BRT_BAND_HIGH), BRT_MATRIX_HIGH, "点阵亮度 HIGH=0x0F");
    EQ(AdcBrt_MatrixIntensityOf((Brt_Band_t)99), BRT_MATRIX_MID, "非法档位兜底 MID");
    /* 单调性与寄存器范围：低<中<高，且都在 0x0~0xF 内 */
    if (!(BRT_MATRIX_LOW < BRT_MATRIX_MID && BRT_MATRIX_MID < BRT_MATRIX_HIGH))
    {
        printf("FAIL 点阵亮度必须单调递增\n");
        s_fail++;
    }
    if (BRT_MATRIX_HIGH > 0x0FU)
    {
        printf("FAIL 点阵亮度超出 0x0~0xF\n");
        s_fail++;
    }

    printf("== 结果: %d/%d 通过 ==\n", s_total - s_fail, s_total);
    return s_fail ? 1 : 0;
}
