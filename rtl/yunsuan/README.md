# VDot64b —— 64-bit 向量点积运算核

团队 **AsyncSys** · 2026 CIE 全国 RISC-V 高水平创新及应用大赛（香山社区赛道）
题目：面向边缘 AI 大语言模型推理的 RISC-V 自定义 vdot 指令设计（香山昆明湖 V2）

## 一、功能

一个 64-bit 数据模块（数据通路对齐 yunsuan 既有 `VIMac64b`）实现 widening 向量点积：

| 输入 | 归约 | 输出 |
|---|---|---|
| 8 × int8  (vs1×vs2) | 每 4 个乘积归约为一组（factor=4） | 2 × int32 |
| 4 × int16 (vs1×vs2) | 每 2 个乘积归约为一组 | 2 × int32 |

- 支持 **signed / unsigned / signed×unsigned（mixed）** 三种符号组合；
- `isMacc` 时把点积结果与旧 `vd`（2×int32）做二补数 widening 累加（`vmacc` 语义），否则覆盖（`vmul` 语义）；
- 流水：**输入拍 S0 → 点积 S1 → 与 oldVd 对齐并累加 S2**，共 2 个寄存器拍；
- 接口（`fire/info/srcType/vs1/vs2/oldVd/highHalf/isMacc/isSub/widen/isFixP`）与 `VIMac64b` 完全对齐，可在 `VIMacU` wrapper 中按自定义 `fuOp` 直接例化。

> 当前为 factor=4（8:4，与 ARM SDOT / SiFive vqdot 一致的产业主流形态）。官方赛题示意的 8:1 单标量基线可通过把归约树拓扑做成可配置来支持，两者共享同一乘法器阵列。

## 二、在香山工程中的落点

把本目录文件放入 `xs-env/XiangShan/yunsuan/`（YunSuan 运算库）：

```
rtl/yunsuan/VDot64b.scala
  -> yunsuan/src/main/scala/yunsuan/vector/vectorIMAC/VDot64b.scala
rtl/yunsuan/VDot64bSpec.scala
  -> yunsuan/src/test/scala/yunsuan/vector/VectorALU/VDot64bSpec.scala
```

## 三、单元测试（chiseltest + Scala golden）

注意 YunSuan 用 mill 0.11.1，**必须用 JDK 17**（JDK 21 会报 bad constant pool index）。

```bash
cd $VDOT_HOME/xs-env/XiangShan/yunsuan
export JAVA_HOME=/team/asc/sun.yingjie/opt/jdk-17.0.21+9
export PATH=$JAVA_HOME/bin:$PATH
mill -i YunSuan.test.testOnly yunsuan.vectortest.mac.VDot64bSpec
```

覆盖：1 组定向边界（非 macc / macc-old0 / 旧值高低位 / 全 FF）+ 5 组各 500 条随机
（s8、u8、mixed8、s16、u16），每条都覆盖 macc 与非 macc。**全部通过（0 失败）**。

## 四、关键设计点与排障记录

1. **每个乘积先归一到 SInt(32) 再进加法树**：Chisel 中 `SInt+SInt` 结果位宽取最大操作数位宽，
   统一到 32 位可让全部加法在 32-bit 二补数下回绕，与软件/ISA 语义一致。
2. **无符号元素高位补 0、有符号元素复制符号位扩 1 位**（`elemS`），保证乘法符号正确。
3. **排障（重要）**：累加模式曾出现"低 32 位固定 +1、非 macc 全对"。
   逐层引出 S2 流水信号后定位：**RTL 完全正确，是 Scala golden 的问题** ——
   golden 对 w=32 的 oldVd 元素做符号修正时用了 JVM Int 移位 `1 << 32`，
   而 Int 移位位数会 `mod 32`，`1<<32` 退化成 `1<<0=1`，导致负的 int32 多减 1。
   修复：符号扩展全程用 Long（`1L << w`）。该坑在写 NEMU golden 时同样要避免。
