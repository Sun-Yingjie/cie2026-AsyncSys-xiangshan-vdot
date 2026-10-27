# 面向边缘 AI 大语言模型推理的 RISC-V 自定义 vdot 指令设计

**2026 CIE 全国 RISC-V 高水平创新及应用大赛 · 香山社区赛道**

| 项 | 内容 |
|---|---|
| 团队名 | **AsyncSys** |
| 作者 | 孙英杰（兰州大学 信息科学与工程学院） |
| 基线处理器 | 香山「昆明湖」V2（`kunminghu-v2`，RVV 1.0，VLEN=128） |
| 命题 / 社区 | 北京开源芯片研究院（香山）· OpenXiangShan |
| 当前状态 | **三阶段全部完成，Difftest 全绿；支持 INT8/INT4 两档点积、全 LMUL；GEMV 端到端 2.82x/3.06x；面积 5.4k 门** |

## 1. 项目简介

在香山昆明湖 V2 的向量乘累加单元（**vimac**）上新增一条融合「**子元素乘法 → 符号扩展 → 4 项归约 → 水平点积累加**」的自定义向量点积指令 **vqdot**（对标 SiFive 官方提案 `Xsfvqdotq`），把量化 LLM 推理（W8A8 / W4A4）中 GEMV / 线性层的点积内核，从标准 RVV 多条指令融合为单条，提升算力密度、降低取指与访存开销。

**为什么标准 RVV 不够**：RVV 1.0 没有跨元素的融合点积，INT8 标准实现需 10 条计算指令（含 3 次置换、2 级加法树），INT4 需约 36 条（提取 4bit + 符号扩展）；`vqdot` 把「子元素乘法 + 符号扩展 + 树状累加」压进一条指令、在一个 64bit 模块内完成。

## 2. 指令语义与编码

统一 OP-V：`opcode=0x57`、`funct3=010`（OPMVV）、`vm=1`；`vd=[11:7]`、`vs1=[19:15]`、`vs2=[24:20]`。

| 指令 | funct6 | 基础字 | 语义 |
|---|---|---|---|
| `vqdot.vv`   | 101100 | `0xB2002057` | 有符号 × 有符号 |
| `vqdotu.vv`  | 101000 | `0xA2002057` | 无符号 × 无符号 |
| `vqdotsu.vv` | 101010 | `0xAA002057` | `vs2` 有符号 × `vs1` 无符号 |

**同一条指令字，按 vtype 的 SEW 选择两种数据通路**：

| 配置 | SEW | 每元素 packed | 累加器 | 单条指令处理 |
|---|---|---|---|---|
| **INT8**（提案已定义） | 32 | 4 × int8 | int32 | 16 个 int8 → 4 个 int32 |
| **INT4**（本项目扩展，SiFive 预留） | 16 | 4 × int4 | int16 | 32 个 int4 → 8 个 int16 |

- 内部：`FuType.vimac`、`VimacOpcode.vdot=111`、`FMT.VVV`、`UopSplitType.VEC_VVV`（LMUL=1 → 1 个 uop，LMUL=L → L 个 uop）。
- INT4 是 SiFive 提案第 4 页明确"预留但未定义"的未来变体（子元素位宽 = SEW/4），构成本项目的核心自主创新点。

## 3. 目录结构

```
rtl/
  yunsuan/                                  # 运算核（yunsuan 子工程）
    VDot64b.scala                           # 参数化点积核（INT8/INT4，S0 合并 oldVd，两级流水）
    VDot64bSpec.scala                       # treadle 单测（1800 组）
    VimacOpcode.scala · yunsuan_package.scala
  xiangshan/
    wrapper/VIMacU.scala                    # 例化 VDot64b、输出选择、MGU eew（=SEW）
    decode/{VecDecoder,DecodeUnit}.scala    # opmvv 译码表、vmaInsts 登记 isDependOldVd
  rocket-chip/Instructions.scala            # VQDOT BitPat
nemu/vqdot.h                                # NEMU golden（按 SEW 分支 INT8/INT4）
test/
  vqdot.{c,asm.S}                           # INT8 功能测试（含 m2）
  vqdot4/                                   # INT4 功能测试（183 用例）
benchmark/
  vbench.{c,asm.S}                          # INT8 点积延迟
  vbench4/                                  # INT4 点积延迟
  gemv/                                     # INT8 GEMV 线性层
  gemv4/                                    # INT4 GEMV（W4）线性层
syn/                                        # DC 综合脚本与面积时序报告
docs/ · test-logs/
```

## 4. 进度与结果

- [x] **阶段一：环境部署** —— 昆明湖 V2 MinimalConfig + NEMU，hello 跑通 Difftest（550 指令零 mismatch）。
- [x] **阶段二：vdot 实现（INT8，全绿）** —— 运算核、wrapper、译码、NEMU golden；定向 + 边界 + 300 组随机，HIT GOOD TRAP、零 mismatch。
- [x] **阶段三：协同仿真与评估**
  - [x] 标准 RVV 基线（INT8 10 条 / INT4 36 条）与延迟对比。
  - [x] **LMUL>1**：无需改 RTL，由 uop 拆分（`VEC_VVV→numOfUop=lmul`）天然支持；LMUL=2 实测 8 lane 全对。
  - [x] **INT8 GEMV**：4×4 tile + 权重重排 + 输入广播，85 cyc vs 240 cyc，**2.82x**。
  - [x] **INT4 扩展**：参数化 VDot（INT8/INT4 两套组合逻辑共享流水），1800 组 treadle + 183 用例全绿，点积 59 vs 110 cyc，**1.86x**。
  - [x] **INT4 GEMV（W4）**：8×4 tile（单条 8 输出），83 cyc vs 254 cyc，**3.06x**。
  - [x] **面积与时序**：DC 综合 VDot64b（SMIC 0.18um），**40,748 µm² ≈ 5.4k NAND2 门**，关键路径 1.77ns。

### 关键结果汇总

| 维度 | vqdot | 标准基线 | 加速比 |
|---|---|---|---|
| INT8 点积·端到端 | 70 cyc | 114 cyc | **1.63x** |
| INT8 点积·纯计算 | 8 cyc | 37 cyc | **4.62x** |
| INT8 GEMV 线性层 | 85 cyc | 240 cyc | **2.82x** |
| INT4 点积·端到端 | 59 cyc | 110 cyc | **1.86x** |
| INT4 GEMV 线性层 | 83 cyc | 254 cyc | **3.06x** |
| 面积 | 5.4k NAND2 门 / 关键路径 1.77ns | — | — |

## 5. 如何复现（可移植，路径均用环境变量）

约定：`source ~/vdot_env.sh`（定义 `VDOT_HOME`、代理开关）；`source $VDOT_HOME/xs-env/env.sh`。

```bash
# (a) 运算核单测（JDK17 + mill 0.11.1 + treadle）
export JAVA_HOME=<JDK17>; export PATH=$JAVA_HOME/bin:$PATH
source ~/vdot_env.sh; proxy_on
cd $VDOT_HOME/xs-env/XiangShan/yunsuan
mill -i YunSuan.test.testOnly yunsuan.vectortest.mac.VDot64bSpec

# (b) NEMU golden（根目录、export NEMU_HOME）
cd $VDOT_HOME/xs-env/NEMU && export NEMU_HOME=$PWD && make -j32

# (c) 测试程序编译（以 vqdot4 为例，其余 vqdot/vbench/vbench4/gemv/gemv4 同理）
cd $VDOT_HOME/xs-env/nexus-am/apps/vqdot4
make ARCH=riscv64-xs CROSS_COMPILE=riscv64-unknown-elf-

# (d) 仿真（JDK21；改 yunsuan/wrapper 后先 rm -rf build/rtl 再重编）
bash $VDOT_HOME/build_simv.sh
bash $VDOT_HOME/run_vqdot.sh    # INT8 功能
bash $VDOT_HOME/run_vqdot4.sh   # INT4 功能
bash $VDOT_HOME/run_vbench.sh   # INT8 延迟
bash $VDOT_HOME/run_vbench4.sh  # INT4 延迟
bash $VDOT_HOME/run_vgemv.sh    # INT8 GEMV 2.82x
bash $VDOT_HOME/run_vgemv4.sh   # INT4 GEMV 3.06x
```

## 6. 设计要点与最关键的一个坑

- **数据通路**：一个 64bit 模块内，INT8 取 8 个 int8、INT4 取 8 个 int4 子元素乘积 → 符号扩展 → 每 4 项归约为 1 个累加器；`S0` 组合 `pre = wrap(dot + oldVd)`，两级流水，与 VIMac64b 对齐。
- **最关键的坑（已修复）**：香山在发射/分派侧有「**忽略 old vd 尾元素**」优化——当指令不依赖旧 vd、且 `vl=vlmax`、`vm=1`/`vta` 时省略 old vd 读取并喂 0。乘加指令必须在 `DecodeUnit.vmaInsts` 中登记以声明 `isDependOldVd`，否则 oldVd 被吞。
- **教训**：给香山加新指令，光改译码表和执行单元不够，凡是「按指令类别」的布尔判定（依赖 old vd、narrow、mask、reverse、wakeUp 等）都要逐一登记。

## 7. INT4 溢出权衡与 MVP 裁剪

- **INT4 累加器为 16bit**：单 tile（4 项）范围 [-224,256] 安全；大 N 连续累加（如 N=4096）会超出 16bit，需软件分块后用 32bit 二级归约（INT8 的 32bit 累加器则安全）。
- RTL 运算核无 mask 输入，按 `vm=1` 整向量 active（NEMU 实现了 `vm=0` 但 RTL 暂不支持）；仅 `.vv`（`.vx` 未做）。
- LMUL 支持 m1/m2/m4/m8（uop 拆分自动覆盖，已实测 m2）；无 FPGA，性能与面积以仿真替代。
- 未来工作：INT16（SEW=64）、BF16/FP16、32bit 累加器的 INT4 变体、`.vx`、元素 mask。
