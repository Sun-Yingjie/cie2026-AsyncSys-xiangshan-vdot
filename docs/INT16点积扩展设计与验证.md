# INT16 点积扩展（SEW=64）设计与验证

## 1. 定位与动机

SiFive Xsfvqdotq 提案第 4 页 NOTE 给出了点积子元素宽度随 SEW 的映射：

| SEW | 子元素 | 累加器 | 一个 64bit 模块 |
|---|---|---|---|
| 16 | int4 | int16 | 4 组 |
| 32 | int8 | int32（标准） | 2 组 |
| **64** | **int16** | **int64** | **1 组** |

本项目在 INT8（标准）、INT4（SEW=16）之后补全 SEW=64 的 INT16 变体，使运算核覆盖 SiFive 映射的全部三档整数点积，验证参数化架构的可扩展性。

语义：

```
vd[i](int64) = old_vd[i] + Σ_{k=0..3} sext64((int16)vs2[i][k]) * sext64((int16)vs1[i][k])
```

128bit 向量 = 2 个 int64 累加器 = 8 个 int16；vlmax=2。

## 2. RTL 实现（VDot64b）

新增 int16 路径（1 组，64bit）：

```scala
val prods16 = Seq.tabulate(4) { k =>
  wrapn(elem16(io.vs1, k, vs1Sign) * elem16(io.vs2, k, vs2Sign), 64)
}
val dot16 = wrapn(prods16(0) + prods16(1) + prods16(2) + prods16(3), 64)
val pre16w: SInt = wrapn(Mux(io.isMacc, dot16 + io.oldVd.asSInt, dot16), 64)
```

关键点：**int16×int16 乘积是 32bit，必须先符号扩展归一到 64bit 再累加**，否则 4 个 32bit 乘积在 32bit 中间和会溢出（最大 4×(32767²) ≈ 2^33）。

模式判定：`isInt16 = SewOH(io.vdType(1,0)).is64`。三档组合逻辑并行、Mux 选择后共享同一两级流水（s1→s2）。

## 3. 踩坑记录

### 3.1 Chisel 匿名块被当成 Wire 的隐式参数列表（编译错误）

最初写法：

```scala
val old16w: UInt = Wire(UInt(64.W))
{                              // ← 紧跟在 Wire(...) 之后
  ...
  old16w := io.oldVd
}
```

`Wire.apply` 第二参数列表是隐式的 `(implicit sourceInfo, compileOptions)`。Scala 把紧随其后的 `{ ... }` 当成这个隐式参数块，于是：
- 块内最后一句 `old16w := io.oldVd`（返回 Unit）被拿去匹配 `SourceInfo`，报 `found Unit, required SourceInfo`；
- 块内引用 `old16w` 本身，报 `recursive value old16w needs type`。

**修复**：去掉匿名块、直接顺序写（只有一组，无需 for/块）；显式类型注解 `val old16w: UInt`。

### 3.2 Scala golden 中 `1L << 64` 回绕（测试假失败）

treadle golden 的 `field()` 用 `(1L << n) - 1` 构造 mask；当 n=64 时，JVM 对 Long 位移量按 64 取模，`1L << 64 == 1L << 0 == 1`，mask 变 0，导致 oldVd 被读成 0，golden 少加了 old。

**修复**：n=64 直接返回原值（或用 BigInt 构造 mask）。

## 4. NEMU golden

vqdot.h 按 `vtype->vsew` 分三档：vsew=2→int8、vsew=1→int4、vsew=3→int16，其余触发 EX_II。累加器统一用 int64_t 容纳，按 accmask（16/32/64bit）回绕。

## 5. 验证结果

- **treadle 单测**：三档各 3×300 组随机 + directed，共 2700 组，EXIT=0 全绿。
- **端到端 Difftest**（vqdot16，3 类型确定性 + 60 组×3 随机）：**183 PASS / 0 FAIL，HIT GOOD TRAP（pc=0x80000538），188,590 条指令零 mismatch**。

## 6. 说明

- INT16（SEW=64）单向量仅 2 个输出，且 64bit 累加器较宽，在边缘 LLM 中不如 INT8/INT4 常用；其主要价值是证明参数化数据通路对 SiFive 三档映射的完整覆盖与零侵入扩展。
- INT16 未单独做标准 RVV 延迟基线：跨两级 widening（e16→e32→e64）的归约序列更长，融合收益相对 int8/int4 只会更高，延迟优势已由 INT8/INT4 两档充分说明。
