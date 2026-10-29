# [2026 CIE 大赛] AsyncSys — 面向边缘 AI 大语言模型推理的 RISC-V 自定义 vdot 指令设计

**团队**：AsyncSys　**学校**：兰州大学　**指导教师**：何安平　**队员**：孙英杰
**作品仓**：https://github.com/Sun-Yingjie/cie2026-AsyncSys-xiangshan-vdot
**基础架构**：香山「昆明湖 V2」（XiangShan `a4eeaabb4c` / NEMU `6a05eea7`），VLEN=128，原生 RVV 1.0

---

## 一、项目概述

RVV 1.0 没有融合点积指令，标准向量实现一个点积需要 `vwmul + 多次 slide/gather + vadd` 约 10 条指令，且是跨元素归约、串行依赖链长。本项目在昆明湖 V2 上新增融合点积指令 **vqdot**（对标 SiFive Xsfvqdotq 提案），单条指令融合"**子元素乘法 → 符号扩展 → 加宽 → 树形累加**"，并扩展支持 **INT4 / INT8 / INT16 三档整数点积**，覆盖 SiFive 提案预留的 SEW 变体。在此基础上，进一步**补全向量（VF）调度器的发射队列唤醒，激活香山既有的 `bypass2` 向量专用旁路**，把向量累加的"结果→再消费"环路从 7 拍压到 3 拍。

## 二、三阶段完成情况

- [x] **阶段一：环境部署** —— 对齐 kunminghu-v2 锁定 commit，编译 NEMU 参考模型，VCS 编译 MinimalConfig simv，hello 二进制 Difftest 零 mismatch、HIT GOOD TRAP。
- [x] **阶段二：vdot 实现** —— 译码/配置层（rocket-chip Instructions、VecDecoder、XSDecode、DecodeUnit）、运算核 VDot64b（三档参数化、两级流水）、NEMU 参考模型（三档 vsew），端到端 Difftest 全绿。
- [x] **阶段三：协同仿真与评估** —— 标准 RVV 基线 kernel、延迟/吞吐对比、LMUL 扩展、GEMV 线性层、端到端量化 MLP、分档面积综合、微架构深度分析。
- [x] **扩展：向量累加旁路优化** —— 补全 VF 唤醒激活既有 `bypass2`，并修正 VF→mem 跨调度器 store 数据源；累加间隔 7.00→3.00 cycle，三项回归全绿。

## 三、性能结果（VCS 仿真，VLEN=128）

| 场景 | vqdot | 标准 RVV | 加速比 |
|---|---|---|---|
| INT8 点积（端到端，含 load/store） | 70 cyc | 114 cyc | **1.63x** |
| INT8 纯计算累加 | 8 cyc | 37 cyc | **4.62x** |
| INT8 GEMV 线性层 | 85 cyc | 240 cyc | **2.82x** |
| INT4 点积（W4） | 59 cyc | 110 cyc | 1.86x |
| INT4 GEMV（W4 线性层） | 83 cyc | 254 cyc | **3.06x** |
| INT16 点积 | 67 cyc | 83 cyc | 1.24x |
| 2 层量化 MLP（**已含旁路优化**） | **252 cyc** | **527 cyc** | **2.09x** |

> 注：前 6 行为点积指令本身的融合收益（旁路优化前测得）；MLP 行为补全 VF 唤醒、激活 `bypass2` 后实测。旁路优化使 vqdot MLP 从 285→252、标准 RVV MLP 从 685→527——**两类实现的绝对周期都下降**，详见《vdot累加旁路forwarding优化报告》。

## 四、面积与时序（SMIC 0.18um，compile_ultra，均 129 FF，slack MET）

| 配置 | 面积 | 等效门数 |
|---|---|---|
| INT4 | 32,529 µm² | 4.3k |
| INT8 | 40,663 µm² | 5.4k |
| INT16 | 57,428 µm² | 7.6k |
| 合一三档（共享流水） | 109,597 µm² | 14.6k |

合一配置比三档独立之和（130,620）**省 16.1%**，来自共享一套流水寄存器与统一模式 Mux。旁路优化仅激活既有旁路连线，未新增流水级或运算器，对运算核面积无影响。

## 五、微架构分析

- 单指令**执行延迟 2 cyc**（两级流水），完全流水化；
- 无依赖 back-to-back **1.73 CPI**（受 MinimalConfig 前端限制，非运算核瓶颈）；
- 累加同一 vd 的**稳态发射间隔 3.00 cycle**（补全 VF 唤醒、激活 `bypass2` 后；优化前为 7.00 cycle）；
- 标准 RVV 点积序列约 33.4 cyc，**即使最不利完全串行，vqdot 仍快约 4.7x**。

**根因与优化（纠正初判）**：香山本有三级转发（forward / bypass / bypass2），`bypass2` 即向量专用跨执行单元旁路；但上游 `Parameters.scala` 从未给 VF 调度器配置 IQ 唤醒配对，导致 `bypass2` 对整个向量域休眠（源/汇均为 0），vqdot/VMACC/VFMA 等累加都只能走完整写回（约 7 拍）。补全 VF 唤醒组（VFEX0/VFEX2 → VFEX0-4、VLSU0/1）后旁路激活，间隔降到 3 拍；这是**整个 VF 调度器共性问题，非 vdot 特有**。

**本质**：vqdot 把一条约 13 指令的串行归约依赖链，折叠为 1 条两级流水指令；而边缘 LLM 的 GEMV/FFN 是累加密集负载，消费者几乎总是下一条累加指令，因此缩短"结果→再消费"环路（旁路）与融合指令本身同等关键。点积融合收益随单向量内归约子元素数量增长，故低比特量化（W4/W8）最受益。

## 六、目录说明

- `rtl/`：yunsuan 运算核、香山 wrapper/decode、rocket-chip 指令定义；
- `nemu/`：参考模型 vqdot 三档实现；
- `benchmark/`：vbench / vgemv / vmlp / vlat 等；
- `syn/`：面积综合脚本与报告；
- `docs/`：设计与分析报告（含《vdot累加旁路forwarding优化报告》）；
- `test-logs/`：Difftest 全绿日志。

恳请评委老师批评指正！
