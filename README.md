# 面向边缘 AI 大语言模型推理的 RISC-V 自定义 vdot 指令设计

**2026 CIE 全国 RISC-V 高水平创新及应用大赛 · 香山社区赛道**

| 项 | 内容 |
|---|---|
| 团队名 | **AsyncSys** |
| 作者 | 孙英杰（兰州大学 信息科学与工程学院） |
| 基线处理器 | 香山「昆明湖」V2（`kunminghu-v2`，RVV 1.0，VLEN=128） |
| 命题 / 社区 | 北京开源芯片研究院（香山）· OpenXiangShan |
| 当前状态 | **三阶段全部完成，Difftest 全绿；INT4/INT8/INT16 三档点积；GEMV 2.82x/3.06x；端到端 2 层 MLP 2.40x；分档面积 4.3k/5.4k/7.6k 门，合一 14.6k 门；支持 LMUL>1** |

## 1. 项目简介

在香山昆明湖 V2 的向量乘累加单元（**vimac**）上新增一条融合「**子元素乘法 → 符号扩展 → 4 项归约 → 水平点积累加**」的自定义向量点积指令 **vqdot**（对标 SiFive 官方提案 `Xsfvqdotq`），把量化 LLM 推理（W8A8/W4A16）中 GEMV / 线性层的点积内核，从标准 RVV 多条指令融合为单条，提升算力密度、降低取指与访存开销。运算核按 vtype 的 SEW 参数化，覆盖三档整数点积。

**一条指令字、三档精度（由 vtype 的 SEW 选择，硬件按 SEW 复用同一数据通路）**：

| SEW | 子元素 | 每累加器 | 128bit 向量 | 来源 |
|---|---|---|---|---|
| 32 | int8 | 4×int8 → int32 | 16 int8 → 4 int32 | SiFive 标准 |
| 16 | int4 | 4×int4 → int16 | 32 int4 → 8 int16 | SiFive 提案预留变体（本项目实现） |
| 64 | int16 | 4×int16 → int64 | 8 int16 → 2 int64 | SiFive 映射的最高精度变体（本项目实现） |

**为什么标准 RVV 不够**：RVV 1.0 没有跨元素的融合点积，标准实现需多条指令（widening 乘法 + 反复置换 + 多级加法树）；`vqdot` 把「子元素乘法 + 符号扩展 + 加宽 + 树状累加」压进一条指令、在一个 64bit 模块内完成。架构可进一步向 BF16 / FP16 扩展。

## 2. 指令语义与编码（定稿，对标 SiFive）

统一 OP-V：`opcode=0x57`、`funct3=010`（OPMVV）、`vm=1`；`vd=[11:7]`、`vs1=[19:15]`、`vs2=[24:20]`。

| 指令 | funct6 | 基础字 | 语义（SEW=32，每个 32bit 元素 packed 4×int8） |
|---|---|---|---|
| `vqdot.vv`   | 101100 | `0xB2002057` | 有符号：`vd[i] = (int32)old_vd[i] + Σ_4 (int8)vs2·(int8)vs1`（wrapping） |
| `vqdotu.vv`  | 101000 | `0xA2002057` | 无符号：`vd[i] = (int32)old_vd[i] + Σ_4 (uint8)vs2·(uint8)vs1` |
| `vqdotsu.vv` | 101010 | `0xAA002057` | 混合：`vs2` 有符号 × `vs1` 无符号 |

- 内部：`FuType.vimac`、`VimacOpcode.vdot=111`、`FMT.VVV`、`UopSplitType.VEC_VVV`（LMUL=1 → 1 个 uop）；MGU 目标 eew = 输入 SEW（int8→e32 / int4→e16 / int16→e64）。
- 同一指令字，硬件按 `vtype` 的 SEW 选择子元素宽度：SEW=32 为 int8（标准），SEW=16 为 int4、SEW=64 为 int16（参数化运算核，两套/三套组合逻辑共享同一两级流水）。
- SiFive 提案第 4 页 NOTE 明确 SEW=16/64 为「预留但未定义的未来变体」，本项目将其补全，是主要创新点。

## 3. 目录结构

```
rtl/
  yunsuan/                                  # 运算核（yunsuan 子工程）
    VDot64b.scala                           # 64bit 点积运算核（S0 合并 oldVd，两级流水）
    VDot64bSpec.scala                       # treadle 单元测试
    VimacOpcode.scala                       # vdot=111、isMacc 含 vdot
    yunsuan_package.scala                   # VimacType.vdot/vdotu/vdotsu
  xiangshan/
    wrapper/VIMacU.scala                    # 例化 VDot64b、输出选择、MGU eew
    decode/VecDecoder.scala                 # opmvv 译码表
    decode/DecodeUnit.scala                 # vmaInsts 登记 isDependOldVd（关键）
  rocket-chip/Instructions.scala            # VQDOT BitPat 译码模式
nemu/
  vqdot.h                                   # NEMU golden（按 SEW 分支：int8/int4/int16）
  rvv_decode.h / rvv_exec.h / isa-all-instr.h
test/
  vqdot.c / vqdot_asm.S / Makefile          # INT8 功能/正确性测试
  vqdot4/                                   # INT4（SEW=16）功能测试
  vqdot16/                                  # INT16（SEW=64）功能测试
benchmark/
  vbench.c / vbench_asm.S / Makefile        # INT8 点积延迟基准
  vbench4/                                  # INT4 点积延迟基准
  gemv/                                     # INT8 量化 GEMV 端到端基准
  gemv4/                                    # INT4 量化 GEMV 端到端基准
  mlp/                                      # 端到端 2 层量化 MLP 推理（2.40x）
syn/                                        # DC 综合脚本/设计/面积时序报告
docs/  ·  test-logs/
```

## 4. 进度

- [x] **阶段一：环境部署与验证** —— 昆明湖 V2 MinimalConfig + NEMU，hello 跑通 Difftest（550 指令零 mismatch，HIT GOOD TRAP）。
- [x] **阶段二：vdot 设计实现（MVP，全绿）**
  - [x] 运算核 `VDot64b`：yunsuan treadle 单测（随机 oldVd/isMacc）全绿。
  - [x] wrapper `VIMacU` 接入 + MGU 目标 eew=e32。
  - [x] 译码（`Instructions.scala` + `VecDecoder.scala` + `VimacType/VimacOpcode`）。
  - [x] **关键属性登记**：`DecodeUnit.vmaInsts` 加入 vqdot → `isDependOldVd=true`（见 §6）。
  - [x] NEMU golden。
  - [x] **端到端 Difftest 全绿**：定向 + 边界 + 3 类型×100 组随机，HIT GOOD TRAP、零 mismatch。
- [x] **阶段三：协同仿真与评估**
  - [x] NEMU 与 RTL 协同仿真（Difftest）。
  - [x] 标准 RVV 基线（10 条计算指令：vwmul + 2 级加法树 + 置换 + 加宽归约）。
  - [x] 延迟对比（见下表），报告见 docs《阶段三延迟加速比分析报告》。
  - [x] **LMUL>1 扩展**：无需改 RTL，由 uop 拆分（`VEC_VVV→numOfUop=lmul`）天然支持；LMUL=2（32 INT8→8 INT32）实测 8 lane 全对，报告见 docs《LMUL多寄存器扩展原理与验证》。
  - [x] **GEMV 线性层端到端**：vqdot 用于量化线性层（4×4 tile + 权重重排 + 输入广播），实测 vqdot 85 cyc vs 标准 RVV 240 cyc，**端到端 2.82x**，报告见 docs《GEMV线性层端到端加速报告》。
  - [x] **分档面积与时序评估（compile_ultra，SMIC 0.18um，均 129 FF）**：INT4 **32,529 µm²/4.3k 门**、INT8 **40,663/5.4k 门**、INT16 **57,428/7.6k 门**；合一三档复用 **109,597 µm²/14.6k 门**（比三档独立之和省 16%，共享一套流水）；全部 2ns 周期 slack MET。详见 docs《精度面积性能三维权衡分析》、syn/modes/。
  - [x] **INT4 点积扩展（SEW=16，W4 量化）**：补全 SiFive 提案预留的 INT4 变体，运算核参数化；3 类型×60 组随机 Difftest 全绿；点积 59 vs 110 cyc（1.86x）；**INT4 GEMV（W4 线性层）83 vs 254 cyc，3.06x**，报告见 docs《INT4点积扩展设计与验证》《INT4_GEMV线性层端到端加速报告》。
  - [x] **INT16 点积扩展（SEW=64）**：补全 SiFive 映射的最高精度变体，4×int16→int64；运算核与 NEMU 三档参数化；3 类型×60 组随机 Difftest 全绿（183 PASS、18.8 万指令零 mismatch）。至此 SiFive 三档整数点积（INT4/INT8/INT16）全部打通。
  - [x] **端到端量化 MLP 推理**：用参数化 GEMV kernel（M=4、N 可变）堆叠 2 层 MLP（16→8→4，含 ReLU + 量化），对标 LLM FFN；vqdot 285 cyc vs 标准 RVV 685 cyc，**端到端 2.40x**，报告见 docs《端到端量化MLP推理加速报告》。

**阶段三延迟结果（VLEN=128，16×INT8）**：

| 维度 | vqdot | baseline | 加速比 |
|---|---|---|---|
| 端到端 kernel（含 load/store） | 70 cyc | 114 cyc | **1.63x** |
| 纯计算（累加延迟） | 8 cyc | 37 cyc | **4.62x** |

## 5. 如何复现（可移植，路径均用环境变量）

约定：`source ~/vdot_env.sh`（定义 `VDOT_HOME`、代理开关）；`source $VDOT_HOME/xs-env/env.sh`。

**(a) 运算核单元测试（JDK17 + mill 0.11.1 + treadle）**
```bash
export JAVA_HOME=<你的 JDK17 路径>; export PATH=$JAVA_HOME/bin:$PATH
source ~/vdot_env.sh; proxy_on
cd $VDOT_HOME/xs-env/XiangShan/yunsuan
mill -i YunSuan.test.testOnly yunsuan.vectortest.mac.VDot64bSpec
```

**(b) NEMU golden**
```bash
cd $VDOT_HOME/xs-env/NEMU
export NEMU_HOME=$VDOT_HOME/xs-env/NEMU
make -j32    # 产物 build/riscv64-nemu-interpreter-so
```

**(c) 功能测试 / 延迟基准**
```bash
cd $VDOT_HOME/xs-env/nexus-am/apps/vqdot    # 功能
make ARCH=riscv64-xs CROSS_COMPILE=riscv64-unknown-elf-
cd $VDOT_HOME/xs-env/nexus-am/apps/vbench   # 延迟基准
make ARCH=riscv64-xs CROSS_COMPILE=riscv64-unknown-elf-
```

**(d) 仿真（JDK21，改 RTL 后先删 build/rtl 再重编）**
```bash
bash $VDOT_HOME/build_simv.sh    # 产物 build/simv
bash $VDOT_HOME/run_vqdot.sh     # INT8 功能：HIT GOOD TRAP、零 different
bash $VDOT_HOME/run_vqdot4.sh    # INT4 功能（SEW=16）
bash $VDOT_HOME/run_vqdot16.sh   # INT16 功能（SEW=64）
bash $VDOT_HOME/run_vbench.sh    # 点积延迟：端到端 + 纯计算周期与加速比
bash $VDOT_HOME/run_vgemv.sh     # INT8 GEMV 线性层：端到端 2.82x
bash $VDOT_HOME/run_vgemv4.sh    # INT4 GEMV 线性层：端到端 3.06x
bash $VDOT_HOME/run_vmlp.sh      # 端到端 2 层 MLP 推理：2.40x
# 面积综合：cd $VDOT_HOME/syn_vdot && dc_shell -f syn2.tcl（5.4k 门，见 syn/）
```

## 6. 设计要点与最关键的一个坑

- **数据通路（参数化三档）**：一个 64bit 模块内按 SEW 分组——int8 为 2 组（每组 4×int8 归一到 32bit）、int4 为 4 组（每组 4×int4 归一到 16bit）、int16 为 1 组（4×int16 乘积归一到 64bit 再累加）；乘积统一归一到累加器位宽后做加法树，保证组间进位不串扰；`S0` 组合 `pre = wrap(dot + oldVd)`，两级流水 `s1→s2`，与 VIMac64b 对齐。三档组合逻辑按 SEW 选择后共享同一套流水寄存器。
- **最关键的坑（已修复）**：香山在发射/分派侧有「**忽略 old vd 尾元素**」优化——当指令不依赖旧 vd、且 `vl=vlmax`、`vm=1`/`vta` 时，省略 old vd 读取并喂 0。乘加指令即使元素全 active 也必须声明 `isDependOldVd`，否则 oldVd 被吞。新指令必须在 `DecodeUnit.vmaInsts` 中登记。详见 docs《oldVd累加bug根因与修复》《新指令接入香山完整清单》。
- **教训**：给香山加新指令，光改译码表和执行单元不够，凡是「按指令类别」的布尔判定（依赖 old vd、narrow、mask、reverse、wakeUp 等）都要逐一登记。

## 7. MVP 裁剪说明

- RTL 运算核无 mask 输入，按 `vm=1` 整向量 active（测试程序固定 vm=1）；NEMU 侧实现了 `vm=0` 但 RTL 暂不支持。
- 仅实现 `.vv`（`.vx` OPMVX 未做）；整数点积覆盖 **INT8/SEW=32、INT4/SEW=16、INT16/SEW=64**（其余 SEW 触发非法指令）。
- INT4 累加器为 16bit、INT16 为 64bit：大 N 累加时 INT4 理论上可能溢出 16bit，需分块或改用更宽累加器（INT4 论文中已注明，作为未来工作）；INT8/INT16 累加器位宽充裕。
- LMUL 支持 m1/m2/m4/m8（由 uop 拆分自动覆盖，已实测 m2；m4/m8 机制相同）。
- 无 FPGA，性能与面积以仿真替代。
