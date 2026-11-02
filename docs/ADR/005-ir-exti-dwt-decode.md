# ADR-005: 红外接收用 EXTI + DWT 时间戳，不用定时器捕获、不用轮询

## 状态

Accepted（2026-11-01）；关联 FR-IR-01~07

## 背景

0038 一体化接收头输出 38kHz 解调后的 NEC 波形：前导 9ms 低 + 4.5ms 高，位宽 560µs / 1690µs，
最短判定单元 560µs。需要把边沿间隔量出来再判位。

两条现成的路都不合适：主循环在本工程里是「采样 → 刷屏 → 游戏 tick → 按键」的串行链，
OLED 走 100% 软件 I2C，单次整帧刷最坏阻塞 ~132ms（见 [ADR-004](004-oled-slice-flush.md)，
分片刷后仍有 8.4ms/片）；而一个 NEC `0` 位只有 1.12ms 全周期。轮询只要错过一次边沿，
整帧就废。

## 决策

PB12 配 `GPIO_MODE_IT_RISING_FALLING`（引脚 12 与 PA12 同号不同口，
互不影响），ISR 只读 `DWT->CYCCNT`（72MHz，13.9ns）算微秒间隔，喂给纯 C 状态机
`IrDecode_Edge()`；解出完整帧后压入 4 深度环形队列，主循环 `IrRx_Poll()` 取。

分工边界：ISR 内**只做时间戳与状态机**，不做 RTT 打印、不做游戏动作、不调 HAL 延时；
动作派发全在主循环 `ir_dispatch()`（`User/main.c`）。解码器 `User/ir/ir_decode.c`
不依赖 HAL / RTT，可在 host 上单测（`tests/test_ir_decode.c`）。

## 否决项

1. **主循环轮询**：OLED 软件 I2C 刷屏阻塞整帧实测 132.1ms、分片刷单片 8.4ms
   （[ADR-004](004-oled-slice-flush.md)；`MyI2C.c` 早年注释里的 "~13ms" 从未实测，已在
   该 ADR 订正），而 NEC 最短判定单元 560µs、一个 `0` 位全周期仅 1.12ms——
   任何口径下阻塞都大于位宽，缺边沿即丢整帧，**不采用**。
2. **TIM 输入捕获**：硬件捕获精度（ns 级）对 560µs 位宽是过剩；代价是要启用
   `HAL_TIM_MODULE`、Makefile 加入 `hal_tim.c` / `hal_tim_ex.c`（flash +3~5KB）、
   处理 CCR 溢出，且多一套 HAL 句柄。EXTI 中断延迟抖动 ~1µs 对位宽判定误差 0.2%，
   完全可接受，**不采用**。

## 后果

- DWT 由 `IrRx_Init()` 内幂等自使能并重取 `s_last_cyc` 基准，不依赖
  `SystemClock_72MHz()` 的先后顺序（`MyI2C_Init()` 会清零 CYCCNT）。
- CYCCNT 32 位 @72MHz 每 59.6s 回绕，全部用无符号差值处理。
- EXTI 回调唯一入口 `HAL_GPIO_EXTI_Callback`（`User/ir/ir_rx.c`）目前只服务 PB12，
  回调首行按 `GPIO_Pin` 分流；将来若给别的引脚开 EXTI（按键是 20ms 轮询，不走这里），
  必须在此处补分支，否则边沿会被吞。
- 升级到需要更短码元（如 RC5 的 889µs 半位）的协议时，需重新评估捕获路径。
- 解码器要求帧前存在 ≥12ms 空闲（`ir_decode.c` 的 IDLE 判据）。NEC 标准帧间隔约 108ms 安全；
  若遇到连发式遥控器（帧间隔 <12ms），会稳定丢帧——真机校准若发现"按住丢帧"，先量帧间隔。
