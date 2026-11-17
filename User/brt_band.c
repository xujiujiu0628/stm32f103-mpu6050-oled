/**
  ******************************************************************************
  * @file    brt_band.c
  * @brief   亮度滞回档位纯函数实现（无 IO / 无 HAL，host 直测）
  ******************************************************************************
  */
#include "brt_band.h"

Brt_Band_t AdcBrt_NextBand(uint16_t mv, Brt_Band_t cur)
{
    /* 越级直入规则: 目标域无条件可达（快拧场景），滞回只作用于边界附近。
       即 HIGH 只需 mv>=HIGH_ENTER, 与 cur 无关; LOW/MID 交界处用双沿。 */
    if (mv >= BRT_MV_HIGH_ENTER)
    {
        return BRT_BAND_HIGH;
    }

    switch (cur)
    {
    case BRT_BAND_LOW:
        /* 出 LOW 需爬过 1150（滞回上沿） */
        return (mv >= BRT_MV_MID_ENTER) ? BRT_BAND_MID : BRT_BAND_LOW;

    case BRT_BAND_MID:
        /* 双向各留 150mV 带：下沿 1000 出 LOW，上沿 2650 进 HIGH */
        if (mv <= BRT_MV_MID_EXIT_DN)
        {
            return BRT_BAND_LOW;
        }
        if (mv >= BRT_MV_MID_EXIT_UP)
        {
            return BRT_BAND_HIGH;
        }
        return BRT_BAND_MID;

    case BRT_BAND_HIGH:
        /* 出 HIGH 需掉破 2650（滞回下沿, 低于进入点） */
        return (mv < BRT_MV_MID_EXIT_UP) ? BRT_BAND_MID : BRT_BAND_HIGH;

    default:
        return BRT_BAND_MID;    /* 非法 cur 兜底回默认档 */
    }
}

uint8_t AdcBrt_ContrastOf(Brt_Band_t band)
{
    switch (band)
    {
    case BRT_BAND_LOW:  return BRT_CONTRAST_LOW;
    case BRT_BAND_HIGH: return BRT_CONTRAST_HIGH;
    case BRT_BAND_MID:
    default:            return BRT_CONTRAST_MID;
    }
}

uint8_t AdcBrt_MatrixIntensityOf(Brt_Band_t band)
{
    switch (band)
    {
    case BRT_BAND_LOW:  return BRT_MATRIX_LOW;
    case BRT_BAND_HIGH: return BRT_MATRIX_HIGH;
    case BRT_BAND_MID:
    default:            return BRT_MATRIX_MID;
    }
}
