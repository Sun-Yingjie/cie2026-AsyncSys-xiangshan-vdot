# 面向边缘 AI 大语言模型推理的 RISC-V 自定义 vdot 指令设计

**2026 CIE 全国 RISC-V 高水平创新及应用大赛 · 香山社区赛道**

- 团队名：AsyncSys
- 成员：孙英杰（兰州大学 信息科学与工程学院）
- 基线处理器：香山「昆明湖」V2（kunminghu-v2，RVV 1.0，VLEN=128）
- 命题/社区：北京开源芯片研究院（香山）· OpenXiangShan

## 一句话简介

在香山昆明湖 V2 的向量乘累加单元（vimac）上新增一条融合「子元素乘法 → 符号扩展 → 加宽 → 水平点积累加」的自定义向量点积指令 **vdot**，覆盖 INT8/INT16 的 signed/unsigned/mixed（架构可扩 INT4/BF16/FP16），用于把边缘 LLM 量化推理（W8A8/W4A16）里的 GEMV/线性层点积内核从多条 RVV 指令融合为单条，提升算力密度并降低取指与访存开销。

## 目录结构

```
rtl/yunsuan/
  VDot64b.scala       # 核心运算核（放入 yunsuan 主源码，见下）
  VDot64bSpec.scala   # chiseltest 单元测试（Scala golden 参考）
docs/
  vdot核心实现链路与编码设计.md   # 权威施工图：指令编码/改动清单/数据通路/Difftest
test-logs/
  vdot64b_unittest.log           # 单元测试通过日志
```

## 进度

- [x] **阶段一：环境部署与验证** —— 昆明湖 V2 MinimalConfig 编译通过，NEMU 参考模型就绪，hello 跑通 Difftest（550 条指令零 mismatch，HIT GOOD TRAP）。
- [x] **阶段二（进行中）**：
  - [x] 核心运算核 `VDot64b`：INT8/INT16 × signed/unsigned/mixed × widening macc，yunsuan 独立单测 **6 项全过、2500+ 随机用例零失配**。
  - [ ] wrapper `VIMacU` 按内部 opcode 接入 + MGU 目标 eew（INT8=vsew+2）。
  - [ ] 指令译码（`Instructions.scala` BitPat + `VecDecoder.scala` + `VimacType/VimacOpcode`）。
  - [ ] NEMU golden（标准 `rvv/` 目录）。
  - [ ] Difftest 端到端一致性。
- [ ] **阶段三：协同仿真与评估** —— Qwen2.5/Llama3.2-1B W8A8/W4A16 内核加速比、面积/功耗评估。

## 指令编码（定稿）

标准 OP-V（opcode=0x57），funct3=010（OPMVV），复用 RVV 空闲 funct6 段 0x10–0x1F：

| 指令 | funct6 | 语义 |
|---|---|---|
| `vdot.vv`   | 010000 | 有符号点积，vd[i] = Σ vs2·vs1（有符号），widening 累加 |
| `vdotu.vv`  | 010001 | 无符号点积 |
| `vdotsu.vv` | 010010 | vs2 有符号 × vs1 无符号 |

内部编码复用 `FuType.vimac`，`VimacOpcode=111`（当前空闲），不新建功能单元，复用向量发射/访存/rename 流水线。

## 如何复现单测（可移植）

依赖 JDK **17**（yunsuan 的 mill 0.11.1 不兼容 JDK21）。路径均通过环境变量 `$VDOT_HOME` 引用，不写死绝对路径。

```bash
# 1) 放置文件到昆明湖 V2 的 yunsuan 工程
cp rtl/yunsuan/VDot64b.scala \
   $VDOT_HOME/xs-env/XiangShan/yunsuan/src/main/scala/yunsuan/vector/vectorIMAC/
cp rtl/yunsuan/VDot64bSpec.scala \
   $VDOT_HOME/xs-env/XiangShan/yunsuan/src/test/scala/vector/VectorALU/

# 2) 用 JDK17 + 代理跑测试
export JAVA_HOME=<你的 JDK17 路径>
export PATH=$JAVA_HOME/bin:$PATH
source ~/vdot_env.sh && proxy_on   # 纯内网机器经 SSH 反向隧道走代理
cd $VDOT_HOME/xs-env/XiangShan/yunsuan
mill -i YunSuan.test.testOnly yunsuan.vectortest.mac.VDot64bSpec
```

预期：6 个测试项（directed + s8/u8/mixed8/s16/u16）全部通过。

## 设计要点

- 一个 64bit 数据模块：INT8 时 8 元素 → 2 个 int32（4:1 加法树）；INT16 时 4 元素 → 2 个 int32（2:1 加法树）。
- 每个乘积先归一到 SInt(32) 再做加法树，保证全程在 32bit 二补数下回绕、两个 int32 组之间进位不串扰（与 ISA 语义一致）。
- 无符号元素「高位补 0 扩 1 位」、有符号元素「复制符号位扩 1 位」表示为正/负 SInt。
- 2 个寄存器拍延迟（输入拍 → 点积 S1 → S2 与 oldVd 组合累加），与 VIMac64b 流水对齐。
