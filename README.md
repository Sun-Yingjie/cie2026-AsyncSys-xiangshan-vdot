# 面向边缘 AI 大语言模型推理的 RISC-V 自定义 vdot 指令设计

**2026 CIE 全国 RISC-V 高水平创新及应用大赛 · 香山社区赛道**

| 项 | 内容 |
|---|---|
| 团队名 | **AsyncSys** |
| 作者 | 孙英杰（兰州大学 信息科学与工程学院） |
| 基线处理器 | 香山「昆明湖」V2（`kunminghu-v2`，RVV 1.0，VLEN=128） |
| 命题 / 社区 | 北京开源芯片研究院（香山）· OpenXiangShan |
| 当前状态 | **阶段二 MVP 全链路 Difftest 全绿** |

## 1. 项目简介

在香山昆明湖 V2 的向量乘累加单元（**vimac**）上新增一条融合「**子元素乘法 → 符号扩展 → 4 项归约 → 水平点积累加**」的自定义向量点积指令 **vqdot**（对标 SiFive 官方提案 `Xsfvqdotq`），把量化 LLM 推理（W8A8/W4A16）中 GEMV / 线性层的点积内核，从标准 RVV「`vmul` + `vredsum`」多条指令融合为单条，提升算力密度、降低取指与访存开销。架构可向 INT16 / INT4 / BF16 / FP16 扩展。

**为什么标准 RVV 不够**：RVV 1.0 没有跨元素的融合点积，`vqdot` 的价值是把「子元素乘法 + 符号扩展 + 加宽 + 树状累加」压进一条指令、在一个 64bit 模块内完成。

## 2. 指令语义与编码（定稿，对标 SiFive）

统一 OP-V：`opcode=0x57`、`funct3=010`（OPMVV）、`vm=1`；`vd=[11:7]`、`vs1=[19:15]`、`vs2=[24:20]`。

| 指令 | funct6 | 基础字 | 语义（SEW=32，每个 32bit 元素 packed 4×int8） |
|---|---|---|---|
| `vqdot.vv`   | 101100 | `0xB2002057` | 有符号：`vd[i] = (int32)old_vd[i] + Σ_4 (int8)vs2·(int8)vs1`（wrapping） |
| `vqdotu.vv`  | 101000 | `0xA2002057` | 无符号：`vd[i] = (int32)old_vd[i] + Σ_4 (uint8)vs2·(uint8)vs1` |
| `vqdotsu.vv` | 101010 | `0xAA002057` | 混合：`vs2` 有符号 × `vs1` 无符号 |

- 内部：`FuType.vimac`、`VimacOpcode.vdot=111`、`FMT.VVV`、`UopSplitType.VEC_VVV`（LMUL=1 → 1 个 uop）；MGU 目标 eew=e32。
- 128bit 向量 = 16 个 int8 → 4 个 int32；一个 64bit 模块输出 2 个 int32。
- SEW 必须为 32（MVP 由测试程序固定；SEW 合法性判定为待完善项）。

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
  vqdot.h                                   # NEMU golden（SEW=32 packed 8bit）
  rvv_decode.h / rvv_exec.h / isa-all-instr.h
test/
  vqdot.c / vqdot_asm.S / Makefile          # nexus-am 测试程序
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
  - [x] **端到端 Difftest 全绿**：定向（DOT+OLD=108/208/308/408、OLDONLY=100/200/300/400）+ 边界 ss/uu/su + 3 类型×100 组随机（含非零 old），HIT GOOD TRAP、零 mismatch。
- [ ] **阶段三：协同仿真与评估** —— Qwen2.5 / Llama3.2-1B W8A8/W4A16 内核加速比、面积/功耗评估（仿真替代 FPGA）。

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

**(c) 测试程序**
```bash
cd $VDOT_HOME/xs-env/nexus-am/apps/vqdot
make ARCH=riscv64-xs CROSS_COMPILE=riscv64-unknown-elf-
```

**(d) 仿真（JDK21，改 RTL 后先删 build/rtl 再重编）**
```bash
bash $VDOT_HOME/build_simv.sh    # 产物 build/simv
bash $VDOT_HOME/run_vqdot.sh     # HIT GOOD TRAP、零 different
```

## 6. 设计要点与最关键的一个坑

- **数据通路**：一个 64bit 模块内，8 个 int8 各取乘积 → 符号扩展归一到 32bit → 每 4 项归约为 1 个 int32（共 2 个）；`S0` 组合 `pre = wrap32(dot + oldVd)`，两级流水 `s1→s2`，与 VIMac64b 对齐。
- **最关键的坑（已修复）**：香山在发射/分派侧有「**忽略 old vd 尾元素**」优化——当指令不依赖旧 vd、且 `vl=vlmax`、`vm=1`/`vta` 时，省略 old vd 读取并喂 0。乘加指令即使元素全 active 也必须声明 `isDependOldVd`，否则 oldVd 被吞。新指令必须在 `DecodeUnit.vmaInsts` 中登记。详见 docs《oldVd累加bug根因与修复》《新指令接入香山完整清单》。
- **教训**：给香山加新指令，光改译码表和执行单元不够，凡是「按指令类别」的布尔判定（依赖 old vd、narrow、mask、reverse、wakeUp 等）都要逐一登记。

## 7. MVP 裁剪说明

- RTL 运算核无 mask 输入，按 `vm=1` 整向量 active（测试程序固定 vm=1）；NEMU 侧实现了 `vm=0` 但 RTL 暂不支持。
- 仅实现 `.vv`（`.vx` OPMVX 未做）；仅 INT8 / SEW=32。
- 无 FPGA，性能与面积以仿真替代。
