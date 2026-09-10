/*********************************************************************
*                   (c) SEGGER Microcontroller GmbH                  *
*                        The Embedded Experts                        *
*                           www.segger.com                           *
**********************************************************************
*                                                                    *
*        SEGGER RTT * Real Time Transfer for embedded targets        *
*                  https://github.com/SEGGERMicro/RTT                *
*                                                                    *
**********************************************************************

---------------------------END-OF-HEADER------------------------------
Purpose : User configuration file for RTT.
          For available configuration,
          refer to SEGGER_RTT_ConfDefaults.h.

----------------------------------------------------------------------
*/

#ifndef SEGGER_RTT_CONF_H
#define SEGGER_RTT_CONF_H

/*
 * 本工程调参（其余全部走 ConfDefaults 默认值）：
 * 来源: github.com/SEGGERMicro/RTT @ 4d8feab3150f Config/SEGGER_RTT_Conf.h
 * 理由: F103C8 仅 20KB SRAM；默认 3 通道 × 1024B 上行 = 3KB 静态开销过大。
 *       单通道 + 512B 上行缓冲 ≈ 560B 总占用，且 >8s 的挂接宽裕度（0.65s/行 × ~40B）。
 *       下行通道保持默认 16B（无 host→target 输入）；模式保持 NO_BLOCK_SKIP
 *       （缓冲满且无 host 时绝不阻塞 CPU）。
 */

#define SEGGER_RTT_MAX_NUM_UP_BUFFERS     (1)    /* 默认 3 —— 只用 channel 0      */
#define SEGGER_RTT_MAX_NUM_DOWN_BUFFERS   (1)    /* 默认 3 —— 无下行需求          */
#define BUFFER_SIZE_UP                    (512)  /* 默认 1024                     */

#endif
/*************************** End of file ****************************/
