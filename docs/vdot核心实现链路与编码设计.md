# vdot 自定义指令核心实现链路与编码设计（施工图）

> 作者：主力（亲读源码核对）。分支：`kunminghu-v2`（commit a4eeaabb4）。所有路径相对 `xs-env/`。
> 约定：RTL = `XiangShan/`；运算库 = `XiangShan/yunsuan/`；参考模型 = `NEMU/`（kunminghu-v2 分支）。

## 0. 设计哲学与差异化（先立住"为什么"）

| 维度 | rvt（仓库已存在的 `CONFIG_CUSTOM_TENSOR`） | 标准 RVV 1.0 拼法 | **本项目 vdot** |
|---|---|---|---|
| 粒度 | 粗粒度协处理器（matmul/conv/pooling，64位自定义长指令、独立 URAM 与 TCR 配置寄存器） | 细粒度向量指令 | 细粒度向量指令 |
| 寄存器 | 独立 tensor 寄存器 | 标准向量寄存器 v0–v31 | **复用标准向量寄存器** |
| 指令数 | vmul/vwmul + vwredsum 等多条，中间结果反复进出 VRF | **1 条融合** |
| 侵入面 | 另起一套执行核 | — | **最小侵入：复用 vimac 功能单元，只加运算核与译码项** |
| 工具链 | 需专用编程模型 | 标准 intrinsic | 自定义 intrinsic，语义贴 ARM SDOT/SiFive vqdot |

核心收益（书中 3.2/3.3 节"寄存器级融合"为依据）：标准 RVV 做点积要 `vmul → 符号扩展 → 加宽 → 多次 vredsum/vwredsum`，中间结果反复写回/读出 VRF，占用读口写口且产生指令间依赖气泡；vdot 在 VPU 单条指令的数据通路内完成「逐元素乘法 → 符号扩展 → 加宽 → 加法树归约 → 加回累加器」，结果直接回写累加寄存器，**省掉中间结果的 VRF 往返、消除气泡、降低前端指令数与功耗**。

对标：ARM SVE2 SDOT/USDOT（4×int8→int32）、x86 VNNI VPDPBUSD；RISC-V 阵营 SiFive `vqdot`(Xsfvqdotq)、Andes `XAndesVDot`、进迭 `XsmtVdot`（命题方，注意其走 custom-1 opcode）。**本项目走标准 OP-V + 自定义 funct6 路线**（与 SiFive 一致，复用昆明湖完整向量流水线，侵入最小），与进迭的 custom-1 路线形成对照。

## 1. 指令集设计（ISA）

### 1.1 编码格式
标准 RVV 向量算术指令（OP-V, opcode7 = `1010111`），字段：
`funct6[31:26] | vm[25] | vs2[24:20] | vs1[19:15] | funct3[14:12] | vd[11:7] | opcode[6:0]`
- 向量-向量 `.vv`：funct3 = `010`（OPMVV）；向量-标量 `.vx`：funct3 = `110`（OPMVX）。
- vdot 读旧 vd 作累加器（三源），故用 `OPMVV`（其首参 `vdRen=T`，与 `vmacc` 同类）。

### 1.2 funct6 取值（已逐行核对 `rocket-chip/.../Instructions.scala`，funct3=010 下 `010000–011111` 整段空闲）
| 指令 | funct6 | funct3 | 语义 | 内部符号 |
|---|---|---|---|---|
| `vdot.vv  vd, vs2, vs1, vm` | `010000` | 010 | 有符号 int8/int16 子元素点积，累加到 widen 寄存器 | vs2=S,vs1=S |
| `vdotu.vv` | `010001` | 010 | 无符号 | vs2=U,vs1=U |
| `vdotsu.vv` | `010010` | 010 | 有符号×无符号（与标准 `vwmulsu` 符号约定保持一致，编码时核对） | vs2=S,vs1=U |
| 对应 `.vx` | 同上 | 110 | rs1 标量广播 | 同上 |
| 预留 | `010011` | 010 | INT4 dot（4-bit 子元素） | 阶段二 |
| 预留 | `010100–010111` | — | BF16/FP16 dot（fdot） | 阶段三 |

> 必须再核对 funct3=110（OPMVX）下相同 funct6 也空闲；若有占用则在 010000–011111 区段内整体平移。

### 1.3 语义（dot factor 由 SEW 决定，无需额外编码）
- SEW=8（int8）：每 4 个相邻乘积为一组 → 1 个 int32 累加器，**factor=4，EEW=32**；一个 64-bit lane 出 2 个 int32。
- SEW=16（int16）：每 2 个 → 1 个 int32，**factor=2，EEW=32**；一个 64-bit lane 出 2 个 int32。
- SEW=32（int32→int64）：factor=2，EEW=64（阶段二再做）。
- 公式（int8→int32 有符号）：
  `vd_w[i] = old_vd_w[i] + Σ_{k=0..3} sext32(vs1_b[4i+k]) * sext32(vs2_b[4i+k])`
- mask/tail 按 RVV 标准（ta/ma）处理；累加器 vd 恒为有符号。

### 1.4 关键难点（实现时重点攻关）
标准 widening 指令元素数是 /2（SEW×2），但 int8 dot 是 /4（4 个子元素归约为 1）。因此：
- 不能直接套用 `VEC_VVW` 的 lane 两两拼接（`VIMacU.scala:92-96` 是为 widen mul 的 /2 设计）；
- 需在 VDot64b 内部完成"4:1 归约"，并自定义/复用正确的 uop 拆分（让输入按 int8、输出按 int32）；
- 先研究 ARM SDOT 与 SiFive qdot 的元素排布，再决定 `uopSplitType` 复用 `VEC_VVW` 还是新增（**优先级 P0，必须先在单测里验证元素排布**）。

## 2. 改动清单（按文件，自顶向下）

### 2.1 指令 BitPat：`rocket-chip/src/main/scala/rocket/Instructions.scala`
仿照 `VWMACC_VV`（约 817 行）新增：
```scala
def VDOT_VV  = BitPat("b010000???????????010?????1010111")
def VDOTU_VV = BitPat("b010001???????????010?????1010111")
def VDOTSU_VV= BitPat("b010010???????????010?????1010111")
def VDOT_VX  = BitPat("b010000???????????110?????1010111")
def VDOTU_VX = BitPat("b010001???????????110?????1010111")
def VDOTSU_VX= BitPat("b010010???????????110?????1010111")
```

### 2.2 内部 opcode：`yunsuan/.../encoding/Opcode/VimacOpcode.scala`
3 位 opcode 现用 000–110，**111 空闲**：新增 `def vdot = "b111".U`。若需要独立信号则在该 object 加 `def isDot(op)=op==="b111".U`。

### 2.3 内部 fuOpType：`yunsuan/src/main/scala/yunsuan/package.scala` 的 `object VimacType`（364 行起）
fuOpType 9 位 = `pad2 + vs2Sign + vs1Sign + vdSign + widen(bit3) + opcode3`。新增（widen=1，vd=S）：
```scala
def vdot   = LiteralCat(0.U(2.W), INT.S, INT.S, INT.S, FMT.VVW, VimacOpcode.vdot)
def vdotu  = LiteralCat(0.U(2.W), INT.U, INT.U, INT.S, FMT.VVW, VimacOpcode.vdot)
def vdotsu = LiteralCat(0.U(2.W), INT.S, INT.U, INT.S, FMT.VVW, VimacOpcode.vdot) // 符号顺序编码时核对
```
`getOpcode/getFormat/getSrcVdType` 已可复用；若 dot 输出宽度恒为 32（不随 SEW×2），需要单独给出 vdType，可能要扩展 `getSrcVdType`（见 1.4 难点，先做 INT8→INT32 可硬编码验证，再泛化）。

### 2.4 译码表：`src/main/scala/xiangshan/backend/decode/VecDecoder.scala`
- 在 `opmvv` 数组（370 行起）、`opmvx` 数组（444 行起）仿 `VMACC_VV`（383 行）加：
```scala
VDOT_VV   -> OPMVV(T, FuType.vimac, VimacType.vdot,   F, T, F, UopSplitType.VEC_VVV /*或新增DOT切分*/),
VDOTU_VV  -> OPMVV(T, FuType.vimac, VimacType.vdotu,  F, T, F, ...),
VDOTSU_VV -> OPMVV(T, FuType.vimac, VimacType.vdotsu, F, T, F, ...),
```
（`vWen=T,mWen=F`；`src1/src2/src3` 默认三向量源。）
- 复用 `FuType.vimac`，**不新增 fuType**，因此 dispatch/rename/issue/FuConfig 基本无需改动。

### 2.5 运算核（新增）：`yunsuan/.../vector/vectorIMAC/VDot64b.scala`
新建 64-bit 点积核（不要改 `VIMac64b` 的 Booth/Wallace 主路径，避免回归）。INT8 数据通路：
1. 输入 vs1/vs2 各 64 bit，按 8 bit 切 8 个 lane；
2. 8 个有符号/无符号小乘法器（8×8→16，或直接符号扩展到 32 后相乘；**优先用简单乘法器，不必上 Booth**，dot 子元素小）；
3. 加法树：每 4 个乘积 `((p0+p1)+(p2+p3))` → 1 个 int32（3 级加法，体现书中 5.4 行间累加 / 5.5 定点 MAC）；
4. 加回 oldVd 的 2 个 int32 累加器；
5. 输出 64 bit（2 个 int32）。
- INT16：4 个 16-bit 乘法 → 两两加 → 2 个 int32 + oldVd。
- 流水线：参照 `VIMac64b` 分 2–3 级（乘法在 S1，加法树在 S2，累加在 S3），用 `GatedValidRegNext`。
- 先写 yunsuan 单元测试（仿 `src/test/scala/vector/VectorALU/VIMac64bSpec.scala`），**随机对比 C 参考模型**。

### 2.6 功能单元接入：`src/main/scala/xiangshan/backend/fu/wrapper/VIMacU.scala`
- 现有 `val vimacs = Seq.fill(numVecModule)(Module(new VIMac64b))`（64 行）。
- 增加并举例化 `vds = Seq.fill(numVecModule)(Module(new VDot64b))`；
- 当 `opcode === VimacOpcode.vdot` 时，输出/掩码（mgu）选 vdot 通路，否则选原 VIMac64b（**Mux 选择，MVP 可两套都例化先跑通**；面积优化时再做"二选一"）。
- 注意 widen 时 vs2/vs1 的分组（92-96 行）对 dot 不适用，VDot64b 直接吃原始 64 bit 自行切分。
- 旧 `fu/vector/VIMacU.scala` 全文件已注释，忽略。

### 2.7 NEMU 参考模型（Difftest golden，C 侧）
- 编码：`src/isa/riscv64/local-include/encoding.h`（或 rvv 对应宏）加 `VDOT_VV` 等匹配常量。
- 译码：`src/isa/riscv64/instr/rvv/decode.h`，在 funct3=0x2（`vopm`，见 44 行）的 `def_INSTR_TAB` 段仿 `vwmacc`（357 行）加：
  `def_INSTR_TAB("010000 ? ????? ????? 010 ????? ????? ??", vdot);`（`.vx` 放 funct3=110 段）。
- 执行：`instr/rvv/vcompute.h`（仿 `def_EHelper(vwredsum)` 250 行起 / wmacc）新增 `def_EHelper(vdot)`，按 vtype 的 vsew、vl、mask/tail 遍历元素做点积，写回 widen 寄存器。
- 注意：NEMU 是 golden，必须与 RTL 元素排布、饱和/溢出规则严格一致（dot 不做饱和，普通回绕即可）。
- **不要动 `instr/rvt/`**（独立 tensor 扩展，CONFIG_CUSTOM_TENSOR，与本项目无关）。

### 2.8 非法指令/一致性
确认新加 funct6 不再被其它地方判非法（译码表加了 BitPat 即覆盖；搜索 `illegalInstr` / 向量指令默认分支，确认不会冲突）。

## 3. Difftest 与测试链路
1. **yunsuan 单元测试**：VDot64b 随机激励 vs C 黄金模型（最快，先过这关）。
2. **NEMU 编译**：`make riscv64-xs-ref_defconfig && make`（configs 见 `configs/`）。
3. **RTL 编译**：MinimalConfig 优先（省内存/时间），`make emu`（verilator，后置）或 VCS（已有 V-2023.12，优先）。
4. 写裸机测试：用内联汇编/自定义 intrinsic，初始化向量寄存器，执行 `vdot`，与软件点积比对；接入 Difftest（NEMU 为 golden）。
5. 性能：构造点积 kernel（INT8 矩阵-向量、Q8 量化 GEMM 内核），对比 ①标量 ②标准 RVV(vmul+vwredsum) ③vdot 的指令数/cycle；VCS 仿真统计。
6. 端到端（阶段三）：Qwen2.5/Llama3.2-1B 的 W8A8/W4A16 量化线性层（quantize kernel），统计吞吐/能效。

## 4. 三阶段裁剪（单人、MVP 优先）
- **阶段一（跑通环境）**：xs-env 编译、NEMU、昆明湖 MinimalConfig、hello Difftest（与指令无关，先打通）。
- **阶段二 MVP（最小可用指令）**：仅 `vdot.vv`(INT8, signed) + RTL(VDot64b) + NEMU + yunsuan 单测 + Difftest 通过。**先证明全链路**。
- 阶段二增强：vdotu/vdotsu、INT16、`.vx`；微基准对比。
- 阶段三：INT4、BF16/FP16(fdot)、LLM 端到端、面积功耗（DC/综合或 emu 计数器）、intrinsic 与文档。

## 4.5 64bit 数据通路精确对齐（已读源码确认，VDot64b 接口依据）

`wrapper/VIMacU.scala`：
- `dataWidth = cfg.destDataBits`（一个 uop 目标位宽）；`dataWidthOfDataModule=64`；`numVecModule = destDataBits/64`。
- 非 widen：`vs2Split.io.outVec64b`（每 64bit 喂一个模块）；widen：把 outVec32b 奇偶 `groupBy(%2)` 配对成 64bit（line 92-96）。
- 每个 VIMac64b 吃 64bit vs1/vs2/oldVd，输出 64bit vd；`outVd = Cat(vimacs.reverse.map(_.io.vd))`；mgu 用 `outEew = widen ? vsew+1 : vsew`（line 131）做 mask/tail。

**点积在一个 64bit 模块内的数据量（关键结论）：**
| 指令 | 一个模块输入(vs1/vs2) | 一个模块输出 | 源:目标 bit | 拆分类型 |
|---|---|---|---|---|
| vdot int8→int32（factor4） | 8×int8 = 64bit | 2×int32 = 64bit | **等宽（1:1）** | **VEC_VVV**（非widen模块形状） |
| vdot int16→int32（factor2） | 4×int16 = 64bit | 2×int32 = 64bit | 等宽，源元素2× | 复用 widen 通道，需实测 |
| 对照 vwmacc int16→int32 | （逐元素，无归约） | 元素数不压缩 | 标准2:1 widen | VEC_VVW |

**因此 INT8 vdot 的最小改动路线**：
1. 译码用 `VEC_VVV`（不要用 VEC_VVW，也不走 line 92 的奇偶配对）；`VDot64b` 直接吃原始 64bit，内部 8 个 int8 乘法 + 4:1 加法树 → 2 个 int32 + oldVd。
2. 物理上 128bit 源（16 个 int8）→ 128bit 目标（4 个 int32），写回 bit 数与源相同，VRF 读写宽度不变；**唯一需要特殊处理的是 MGU 的目标 eew**：目标元素按 e32（=vsew+2，而非标准 widen 的 vsew+1），active/tail mask 需把源 4 个一组折叠为 1 个目标 active。vtype 建议设**源 SEW=e8**（输入元素按 e8 计 vl）。
3. 累加器 vd 按 e32 寻址，需在 wrapper 里给 `mgu.io.in.info.eew` 一个 dot 专属值（对 int8 = vsew+2；int16 = vsew+1），并验证 tail/ta/ma 行为。
4. 此方案与 SiFive vqdot 思路一致（4×int8→int32），**MVP 先只做 int8 signed**，跑通后再泛化 int16/int4。

## 4.6 yunsuan 独立快速验证（不编译整个香山）
- yunsuan 是独立 mill 项目：`yunsuan/.mill-version=0.11.1`，有 `build.sc`/`Makefile`；单测：`make test`（=`mill YunSuan.test.test`）或单跑 `mill YunSuan.test.testOnly yunsuan.vectortest.mac.VIMac64bSpec`。
- 模板：`yunsuan/src/test/scala/vector/VectorALU/VIMac64bSpec.scala`（chiseltest，Wrapper 包 Module，`enqueueSeq/expectDequeueSeq`，dataType 辅助 s8/u8/s16…、`IMac64bCtrlBundle`、`SrcBundle`、`genVIMac64bInput/Output`）。
- **VDot64b 建议写 Scala 黄金参考**（纯函数按 sew/sign 做点积），随机生成大量 vs1/vs2/oldVd 并自动算期望，比手写 hex 更可靠（避免人为算错）。
- 注意：子代理装的 mill 是 0.12.3，而 yunsuan 要 0.11.1、香山主项目另有要求；请在各项目用其 `.mill-version` 自带/下载对应 mill，不要全局只装一个版本。

## 5. 待确认 / 风险（P0）
1. **dot 元素排布与 uop 拆分（4:1 归约）**——决定 srcType/数据重组，先单测验证。
2. funct3=110 下 funct6 空闲复核；vdotsu 符号顺序。
3. MinimalConfig 是否包含完整 VPU/vimac（若精简掉向量单元，需改 config 或用默认 config）。
4. 无 sudo：JDK21/mill 走用户目录；VCS 优先，verilator 后置。
5. 与标准/厂商 funct6 冲突的书面说明（0x10 段当前空闲；声明为自定义 X-vdot 扩展）。
