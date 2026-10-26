#ifndef __OLED_VIEW_H
#define __OLED_VIEW_H

#include <stdint.h>

/* 显示状态 */
typedef enum {
    OLEDS_RAW = 0,       // 页面 0：六轴原始数据
    OLEDS_EULER,         // 页面 1：欧拉角
    OLEDS_ATTITUDE,      // 页面 2：2D 姿态点图
    OLEDS_TETRIS,        // 页面 3：俄罗斯方块副屏（下一块 + 分数，棋盘在 MAX7219 点阵上）
    OLEDS_OFF,           // 页面 4：关屏
} OledState_t;

/* 刷新节流 */
#define OLEDS_REFRESH_MS   100U   // 10Hz

/* 性能观测探针（第 1 步实测用；只读，供 main.c 的 PERF 心跳汇总打印） */
extern volatile uint32_t g_perf_flush_us;    // 最近一帧整帧刷阻塞耗时 (us)
extern volatile uint32_t g_perf_draw_us;     // 一次 排版+绘制（纯 RAM）耗时 (us)
extern volatile uint32_t g_perf_frames;      // 累计整帧数

/**
 * @brief  初始化：清屏 + 显示状态 0（六轴）+ 启动画面
 */
void OledView_Init(void);

/**
 * @brief  周期性 tick：检查显示节流 + 渲染当前状态
 * @note   应在 10Hz 周期调用（如 10ms tick 累计 10 次）
 */
void OledView_Tick(void);

/**
 * @brief  状态切换（处理按键事件）
 * @param  short_press  1=短按，0=忽略
 * @param  long_press   1=长按，0=忽略
 */
void OledView_HandleKey(uint8_t short_press, uint8_t long_press);

/**
 * @brief  强制刷当前状态（启动/唤醒时用）
 */
void OledView_ForceRefresh(void);

/**
 * @brief  获取当前状态（调试/测试用）
 */
OledState_t OledView_GetState(void);

/* 内部命令（薄封装：补充 My_OLED 未导出的关屏/开屏） */
void OledView_DisplayOff(void);
void OledView_DisplayOn(void);

/* 校准提示覆盖层（FR-CAL-02）：hint=1 全屏覆盖并立即整帧刷
 *（刷屏成本花在校准之前）；hint=0 撤销并 ForceRefresh */
void OledView_ShowCalibHint(uint8_t hint);

#endif
