# vdot 累加旁路（forwarding）优化报告

> 团队 AsyncSys｜项目：面向边缘 AI 大语言模型推理的 RISC-V 自定义 vdot 指令
> 基线：主仓 `79d8dbecf`（kunminghu-v2，INT4/INT8/INT16 三档全绿）
> 最终方案：**VF 唤醒汇只接 VFEX0–4（向量算术），不纳入 VLSU**；store 走完整写回。
> 结论：**累加同一 vd 的稳态发射间隔 6.97 → 3.00 cycle（2.32x），无依赖 1.73 CPI，点积执行延迟仍为 2 cycle；vlat / vqdot / vmlp 全部 HIT GOOD TRAP、Difftest 零 mismatch。**

## 1. 背景与问题

vdot 是面向量化推理的向量点积指令，其语义为"点积 + 累加"：

```
vd[i] = sum_j (vs1[j] * vs2[j]) + old_vd[i]
```

在 GEMV / FFN 等真实 kernel 中，vdot 以**累加链**形式反复出现——后一条指令的目标寄存器就是前一条的目标（同一 `vd`），结果的消费者几乎总是下一条累加指令。因此"结果产生 → 再被消费"的环路延迟，是端到端周期的关键。

基线实测（`vlat`）发现：两条累加同一 `vd` 的 vdot 之间，稳态发射间隔约 **7 拍**，而单指令点积执行延迟仅 2 拍。中间多出的周期来自"写回完整物理寄存器 → 再读"的长环路，旁路（forwarding）未生效。

## 2. 根因：VF 调度器从未配置 IQ 唤醒，bypass2 休眠

香山的发射队列（IQ）有两套结果旁路：

- **bypass1**：通用、零额外延迟的 forward 通路，主要服务整数/标量；
- **bypass2**：向量专用、带寄存（+2 拍）的 forwarding 网络，只有在 `iqWakeUpParams` 中显式配置了唤醒配对的 ExeUnit 才会被生成和激活。

核对 `src/main/scala/xiangshan/Parameters.scala` 的 `iqWakeUpParams` 发现：上游逐字节版本只为整数（ALU、MUL、FPU 等）配置了唤醒组，**整个 VF 向量调度器（VIMac/vdot 所在）没有任何 `WakeUpConfig`**。于是：

- VF 单元的 `hasIQWakeUp=false`，`BypassNetwork` 不为其生成 bypass2；
- 累加消费者无法在结果写回前被提前唤醒，只能等完整写回（约 7 拍）。

这不是 vdot 运算核缺旁路，而是**向量调度器的唤醒配置缺失**，导致既有 bypass2 网络整体休眠。

## 3. 优化方案（配置类，补全 VF 唤醒组）

### 3.1 改动：补全 VF 唤醒组，激活既有 bypass2

文件：`src/main/scala/xiangshan/Parameters.scala`，在 `iqWakeUpParams` 中新增第三组 `WakeUpConfig`：

```scala
// VF fixed-latency arithmetic (VIMac/vdot, VFMA, VIALU, VPPU) -> VF arithmetic
// consumers. Only VF arithmetic Exu are wired as sinks: a vector store (VLSU) reading
// VF is not on the critical path, so it keeps the full writeback path. Including VLSU
// here woke the store too early and paired it with zero-latency forward (2 cycles
// before the result was ready), reading stale data. VFEX4 (Vfdiv/Vidiv) has uncertain
// latency, so it is a sink only.
WakeUpConfig(
  Seq("VFEX0", "VFEX2") ->
  Seq("VFEX0", "VFEX1", "VFEX2", "VFEX3", "VFEX4")
),
```

源/汇选择依据（按 ExeUnit 延迟特性，宁缺毋滥）：

- **只选 VFEX0 / VFEX2 做源**：纯 VF 定延迟 MAC 单元（vdot / VMACC / VFMA 所在），延迟确定，符合"唤醒源必须是 fixed-latency"的约束。
- **VFEX4 只做汇**：含 `Vfdiv / Vidiv`，延迟不确定，不能做源。
- **VFEX1 只做汇**：含 latency-0 的 `VSetRvfWvf` 与写 INT 的 `Vipu`，为降低风险不做源。
- **汇不纳入 VLSU**：见 3.2。

### 3.2 为什么不把 VLSU 纳入提前唤醒（关键正确性边界）

初版曾把 `VLSU0 / VLSU1` 也放进唤醒汇，并在 `EntryBundles.scala` 增加 mem 分支试图让 store 走 `bypass`。但完整 `vqdot` 回归在"向量 load → vmacc/vdot → vector store"序列上出现 mismatch：

- 波形（VPD/VCD）确认：**VIMac 计算完全正确**（输出 [100,200,300,400]）并在正确拍写回新物理寄存器；
- 但 vector store 被提前唤醒，其数据源被配成零寄存的 `forward`，比 VIMac 结果就绪**早约 2 拍**，读到在途结果之前的 0，导致 store 写出 0。

**最终取舍：唤醒汇只保留 VFEX0–4，不纳入 VLSU。** 理由：

1. store 读 vd 不是 GEMV/MLP 的性能瓶颈（瓶颈是点积计算），让它走完整写回即可，正确性有保障——优化前 vqdot 全绿，store 本就走 WB；
2. 跨调度器（VF→mem）的拍数对齐风险高、收益小，不值得为非瓶颈路径冒险。

> 说明：`EntryBundles.scala` 中此前为 VLSU 预留的 mem 分支（`inMemSchd && readVfRf && hasIQWakeUp`）在最终方案里**不会被触发**：移除 VLSU 后，VLSU 的 `numWakeupIQ=0`、`hasIQWakeUp=false`，该分支成为死代码（整数 STA/STD 因 `readVfRf=false` 也不满足）。保留它对功能无影响，并完整记录了"尝试纳入 VLSU → 发现问题 → 移除"的工程判断过程。

### 3.3 为什么不采用运算核内局部 forwarding

在 VIMacU/向量通路内为"同 vd 累加"加局部 forwarding 的方案未采用：补全 VF 唤醒后，VF 内部累加已能通过既有 `bypass2` 正确跑通；且源物理地址（psrc）在 ExuInput 层已被剥离，局部 forwarding 需跨多层引地址、改动大、难回退，非必需。

## 4. 重编与激活确认

强制 re-elaborate（`rm -rf build/rtl`）后重编 `build_simv.sh`，关键确认：

```
[BypassNetwork] HasBypass2SourceExuNum: 2  HasBypass2SinkExuNum: 5  bypass2DataVecSize: 2
HasBypass2SourceExu: Vector(13, 15)
HasBypass2SinkExu:   Vector(13, 14, 15, 16, 17)
```

- 2 个源：exuIdx 13（VFEX0）、15（VFEX2）
- 5 个汇：exuIdx 13–17（VFEX0–4），**不含 VLSU（exuIdx 23/24，numWakeupIQ=0）**
- firtool 生成 `VIMacU.sv` 中 `VDot64b` 计数 = 2（符合预期）
- VCS 编译 `END rc=0`，`BypassNetwork.sv` 中 bypass2 出现 81 次

## 5. 量化收益（vlat 实测）

| 指标 | 基线 | 优化后 | 加速 |
|---|---|---|---|
| 无依赖 CPI | 1.750 | 1.729 | 1.01x |
| 循环有依赖 CPI（含标量开销） | 6.969 | 3.427 | **2.03x** |
| unroll64 cyc/instr | 5.516 | 2.453 | **2.25x** |
| unroll128 cyc/instr | 6.258 | 2.727 | **2.30x** |
| **稳态发射间隔（边际）** | **6.97** | **3.00** | **2.32x** |
| 单指令执行延迟 | 2 | 2 | 不变 |

稳态间隔用边际法测得：`(349−157)/64 = 3.00`，unroll64 与 unroll128 两次独立测量一致。

## 6. 正确性回归（HIT GOOD TRAP、Difftest 零 mismatch）

| 测试 | 结果 | WORKLOAD DONE cycle |
|---|---|---|
| vlat | HIT GOOD TRAP @ pc=0x800006e0，零 mismatch | 50,235 |
| vqdot（INT8 完整，含边界 + 3×100 随机组） | HIT GOOD TRAP @ pc=0x800006f2，零 mismatch | 249,864 |
| vmlp（2 层 MLP） | HIT GOOD TRAP @ pc=0x800006d8，零 mismatch | 43,690 |

- vqdot：`VDOT All Tests PASS (3 boundary + 3x100 random groups)`，305,203 指令、IPC 1.22；此前失败的 `VMACC-REF 100 200 300 400` 已正确。
- vmlp：`MLP vqdot PASS 0 10 20 30`、`MLP baseline PASS`，25,143 指令、43,683 cycle。
- 三项均 HIT GOOD TRAP、Difftest 零 mismatch，点积语义（dot + oldVd）与 store 数据全部正确。

## 7. 面积 / 时序影响

本轮**只改唤醒配置，未新增任何流水级、未新增运算器**：

- 改动只是让既有 `bypass2` 网络生成连线（原本就预留的寄存器/Mux 被激活），不新增运算数据通路；
- VLSU 不纳入唤醒，store 走原有 WB 路径，不新增寄存器。

因此对**点积运算核（VDot64b）的分档面积（int8 40,663 / int4 32,529 / int16 57,428 / 合一 109,598 µm²，compile_ultra 口径）无影响**；对整体 SoC，仅向量旁路网络多生成若干已预留的连线，量级可忽略。

时序方面：VCS 编译通过、仿真行为正常；bypass2 为 +2 拍的寄存输出，并非关键组合路径，不增加关键路径压力。

## 8. 可回退性

改动为纯配置（新增一组 `WakeUpConfig`），回退只需删除该组并重新 `rm -rf build/rtl && bash build_simv.sh`。原始文件备份在服务器 `/tmp/opt_backup/`（`Parameters.scala`、`EntryBundles.scala`）。

## 9. 总结

1. **根因**：不是 vdot 缺旁路，而是整个 VF 调度器在上游从未配置 IQ 唤醒，使既有 `bypass2` 向量专用旁路休眠；累加只能走完整写回（约 7 拍）。
2. **改动**：补全 VF 唤醒组（源 VFEX0/2，汇 VFEX0–4），纯配置、可回退，不动点积语义与频率；并明确把 VLSU 排除在提前唤醒外，store 走 WB 保证正确性。
3. **收益**：稳态累加间隔 6.97 → 3.00（2.32x），无依赖 1.73 CPI，执行延迟仍为 2 cycle；vlat / vqdot / vmlp 全部全绿。
4. **本质**：边缘 LLM 推理的 GEMV/FFN 是"累加密集"负载，缩短"结果→再消费"的环路延迟，比单纯提升单指令吞吐更能直接改善端到端周期——这正是本次旁路优化的价值所在。
