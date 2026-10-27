# INT4 点积扩展：语义、RTL 实现与验证（CIE2026 AsyncSys）

> 团队：AsyncSys　|　指令：VQDOT_VV / VQDOTU_VV / VQDOTSU_VV（SEW=16 变体）
> 基线：香山昆明湖 V2（XiangShan `a4eeaabb4c`）、NEMU `6a05eea7`

## 1. 立项依据：SiFive 提案"预留但未定义"的未来变体

SiFive《Xsfvqdotq》提案只定义了 INT8（SEW=32）一种点积，但其第 4 页明确写道：

> *"Future variants could be defined for different SEW values. SEW=32 gives 8-bit elements, SEW=64 gives 16-bit elements, SEW=16 gives 4-bit elements."*

即点积的子元素位宽 = SEW/4。本项目据此实现 **INT4（SEW=16）** 变体——这是标准提案预留、但尚未定义的合法扩展，构成区别于"照抄提案"的**自主创新点**。

| 配置 | SEW | 每元素 packed 子元素 | 累加器位宽 | 单条指令处理 |
|---|---|---|---|---|
| INT8（提案已定义） | 32 | 4 × int8 | 32 bit | 16 个 int8 → 4 个 int32 |
| **INT4（本项目扩展）** | **16** | **4 × int4** | **16 bit** | **32 个 int4 → 8 个 int16** |

## 2. 指令语义

SEW=16 时，每个 16bit 元素被重解释为 4 个 packed 4bit 子元素，一条指令完成：

```
vd[lane](16b) = old_vd[lane] + Σ_{k=0..3} sign_ext( vs1[lane][k](4b) * vs2[lane][k](4b) )
```

- **VQDOT_VV**：有符号 × 有符号；**VQDOTU_VV**：无符号 × 无符号；**VQDOTSU_VV**：vs2 有符号 × vs1 无符号。
- 128bit 向量 = 8 个 16bit = 32 个 int4，单条指令同时产出 8 个累加器。
- 溢出按 **wrapping（模 2^16）** 处理，与 INT8 的模 2^32 语义一致。

**编码零改动**：INT4 与 INT8 复用完全相同的指令字（`0xB2002057` / `0xA2002057` / `0xAA002057`），硬件在执行阶段根据 `vtype` 的 SEW 选择 INT4/INT8 数据通路。这一设计保证了指令编码的前向兼容，无需占用额外编码空间。

## 3. RTL 参数化设计

### 3.1 核心运算核 VDot64b

`yunsuan/.../vector/vectorIMAC/VDot64b.scala` 参数化，用 SEW 做模式选择：

```scala
val isInt4 = SewOH(io.vdType(1, 0)).is16   // SEW=16 → int4
// INT8：2 组，每组 32bit 累加器（4 个 int8）
// INT4：4 组，每组 16bit 累加器（4 个 int4）
```

- INT8 路径：2 组 `SInt(32)`，每组 4 个 int8 乘积符号扩展到 32bit 后树形累加。
- INT4 路径：4 组 `SInt(16)`，每组 4 个 int4 乘积（经 `sext4` 符号扩展）后树形累加。
- 两套组合逻辑并行例化、用 `Mux(isInt4, ...)` 选择结果，**共享同一套两级流水**（S0/S1 寄存器拍），不额外增加延迟级数。

### 3.2 wrapper 与输出 EEW

`VIMacU.scala`：
- `vdType` 已由 `VIMacSrcTypeModule` 按 SEW 计算（SEW=16 时为 `s16`），直接传入 VDot 触发 INT4 模式。
- **关键修正**：MGU 的输出 EEW 原本对 vdot 固定为 `e32`，会把 INT4 的 8 个 int16 错误地按 4 个 int32 合并。改为：

```scala
// vdot 输出 EEW = 输入 SEW：INT8→e32，INT4→e16
val outEew = Mux(outIsVdot, outVecCtrl.vsew, ...)
```

### 3.3 译码层

VDot 走 `OPMVV`（funct3=010）、`UopSplitType.VEC_VVV`，不属于 widening / intExt / fp 类别。`VecExceptionGen` 的合法性检查中不存在针对 vdot 的 SEW 限制，因此 **SEW=16 与 SEW=32 均合法，译码层无需改动**。

## 4. NEMU 参考模型

`NEMU/src/isa/riscv64/instr/rvv/vqdot.h` 按 `vsew` 分支：

```c
if (vsew == 2) { subbits=8; accmask=0xffffffff; ... }   // INT8
else if (vsew == 1) { subbits=4; accmask=0xffff; ... }  // INT4
else EX_II;
```

新增 `sext4()`（4bit 符号扩展），子元素位宽、累加器掩码参数化，INT4/INT8 共用同一套循环骨架。

## 5. 正确性验证

| 测试 | 内容 | 结果 |
|---|---|---|
| yunsuan treadle 单测 | INT8/INT4 各 3×300 组随机 + directed，共 **1800 组** | **EXIT=0，全通过** |
| `vqdot4` app | 3 组确定性边界（ss/su/uu）+ 60 组 × 3 随机 = **183 个用例** | **183 PASS / 0 FAIL，HIT GOOD TRAP** |
| Difftest | 香山 RTL vs NEMU 逐拍对比 32 个向量寄存器 | **零 mismatch** |

确定性 INT4 用例（A[i]=(i%16)−8，B[i]=7−(i%16)，old=10·lane）lane 0 手工核对：
`(-8)(7)+(-7)(6)+(-6)(5)+(-5)(4) = -56-42-30-20 = -148`，与 RTL/NEMU 一致。

## 6. 延迟对比（SEW=16）

标准 RVV 基线 `base4`：从 16bit packed 数据中用 `vsrl + vand` 提取 4 个 4bit 子元素，`vsll/vsra` 符号扩展到 16bit，4 条 `vmul` 做有符号乘法，再 4 条 `vadd` 向量累加，共 **36 条指令**；vqdot 仅 **1 条**。

| kernel | 端到端延迟（R=20） |
|---|---|
| vqdot4 | **59 cycles** |
| base4 | **110 cycles** |
| **加速比** | **1.86×** |

## 7. 溢出分析与适用边界（重要）

INT4 累加器为 16bit，需注意动态范围：

- **单 tile（4 项）**：int4 ∈ [-8,7]，4 项累加范围 [-224, 256]，16bit 安全。
- **大 N 连续累加**：每累加一次最多 +256/−256。例如 N=4096（累加 1024 次），最大累计可达 262,144，**超出 16bit（±32768）范围，会 wrapping 溢出**。

> 对比 INT8：32bit 累加器即使大 N 也几乎不会溢出。

**适用建议**：INT4/16bit 累加器适合**小 N 或分块（chunked）累加**场景；对大 N 点积，应在每块累加后用 32bit 指令做二级累加，或未来扩展为 32bit 累加器的 INT4 变体（列入未来工作）。这一权衡需在调用方软件中显式处理。

## 8. 与 INT8 配置的系统对比

| 维度 | INT8 (SEW=32) | INT4 (SEW=16) |
|---|---|---|
| 单条指令子元素数 | 16 | **32（2×）** |
| 累加器 | 4 × int32 | 8 × int16 |
| 端到端 vqdot 延迟 | 70 cyc | **59 cyc** |
| 标准基线延迟 | 114 cyc | 110 cyc |
| 端到端加速比 | 1.63× | **1.86×** |
| 大 N 累加器安全性 | 安全 | 需分块（见 §7） |

**结论**：INT4 模式以"更窄累加器"换取了**双倍子元素吞吐**与更高的单指令加速比，契合边缘 AI 中 4bit 量化权重（W4A4/W4A8）的点积需求；代价是 16bit 累加器的动态范围受限，需软件分块。该扩展完整覆盖了"语义定义 → RTL 参数化 → NEMU 协同 → 大规模回归 → 延迟/溢出量化"的全链路，是本项目在 SiFive 提案之外的核心自主贡献。
