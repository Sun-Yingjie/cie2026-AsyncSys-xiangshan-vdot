# vqdot 漏加 oldVd 根因定位与修复（关键调试记录）

> 团队：AsyncSys ｜ 赛道：2026 CIE 全国 RISC-V 大赛·香山社区赛道
> 项目：面向边缘 AI 大语言模型推理的 RISC-V 自定义 vdot 指令（昆明湖 V2）
> 定位时间：阶段二 MVP 调试
> 一句话结论：**vqdot 未被加入译码层的 `vmaInsts` 列表 → `isDependOldVd=false` → 发射/分派的"忽略 old vd 尾元素优化"把 src2(old vd) 改写成 `SrcType.no`/立即数 0，导致运算核读到的 oldVd 恒为 0。**

---

## 1. 故障现象

- 测试程序中，`vqdot.vv v8,v9,v10`（v8 既是目标又是累加源）：
  - 点积部分 `dot = Σ(vs1·vs2)` 结果**正确**；
  - 旧目标 `old v8`（100/200/300/400）**没有被累加**，结果恒等于 `dot + 0`。
- 同一程序里的标准 `vmacc.vv v8,v9,v10` **完全正确**（能读到 old v8=100/200/300/400）。
- 当 old=0 时（边界 ss/uu/su）vqdot 全部通过，因此一度怀疑运算核的 old 锁存时序。

Difftest 典型证据（DOT+OLD：vs1=全 1、vs2=全 2、old=100/200/300/400，期望 108/208/308/408）：

```
pc=0x8000034e
right = 0x000000d0_0000006c ...   # 208 / 108（含 old）
wrong = 0x00000008_00000008       # 8 / 8   （只有 dot，漏 old）
```

---

## 2. 定位过程（自底向上逐层排除）

### 2.1 运算核 VDot64b：证明正确（排除项）
- 路径：`yunsuan/.../vector/vectorIMAC/VDot64b.scala`
- 结构：S0 `pre = wrap32s(dot + Mux(isMacc, oldVd, 0))` 组合 → `s1=RegEnable(pre, fire)` → `s2=RegEnable(s1, fireS1)` → `io.vd=s2`。
- treadle 单测（500 随机/类，随机 oldVd/isMacc）全绿；综合 Verilog 可见 `if(io_fire) s1 <= dot+oldVd`。
- 结论：运算核、流水线、old 锁存无问题。

### 2.2 Chisel printf 探针：把故障边界锁死在"读旧 vd 通路"
在 VDot64b 与 VIMacU wrapper 加探针：

```
[VIMAC IN] op=2(old vmacc) oldVd=0x000001900000012c000000c800000064   # 正确
[VIMAC IN] op=7(vqdot)     oldVd=0x00000000000000000000000000000000   # 全 0
[VDOT F] op=7 oldVd=0 vs1=0x0101...0101 vs2=0x0202...0202             # src0/src1 正确，仅 src2=0
```

- vqdot 进入 FU 时 `src0(vs1)`、`src1(vs2)` 正确，唯独 `src2(old vd)` 恒为 0。
- 标准 vmacc 同一信号正确 → **数据通路本身没坏，是 vqdot 这条指令的 src2 被特殊处理了**。

### 2.3 译码/字段映射/编码：逐行核对（排除项）
- rocket-chip `Instructions.scala`：VQDOT BitPat 与 VMACC_VV 仅 funct6 不同，vm 位为 `?`。
- `VecDecoder.scala` opmvv 表：`VQDOT_VV -> OPMVV(T, FuType.vimac, VimacType.vdot, F,T,F, VEC_VVV)`，与 VMACC_VV 逐字相同。
- `OPMVV`：`vdRen=T → src3=vp`。
- `XSDecode.generate()` 15 列与 `DecodedInst.allSignals` 列序一一对应：`srcType(0)←src1, srcType(1)←src2, srcType(2)←src3`。
- `DecodeUnit`：`lsrc(2)=VD`；`DecodeStage`：`vecRat(2).addr=lsrc(2)`。
- `SrcType`：`vp=0100`；Rename `psrc(2)=Mux1H(srcType(2)(2,1), Seq(fp, vec))`，vp 时正确选 vec 读口。
- 结论：译码给出的 `srcType(2)=vp`、RAT 地址=v8 都正确。

### 2.4 真凶：发射队列的"忽略 old vd 尾元素优化"
`src/main/scala/xiangshan/backend/issue/EntryBundles.scala`（srcIdx==2 即 old vd）：

```scala
val isDependOldVd = entryReg.payload.vpu.isDependOldVd
val ignoreTail   = vlIsVlmax && (vm =/= 0.U || vma) && !isWritePartVd
val ignoreWhole  = (vm =/= 0.U || vma) && vta
ignoreOldVd := srcIsVec && vlIsNonZero && !isDependOldVd && (ignoreTail || ignoreWhole)

srcStatusNext.srcType   := Mux(ignoreOldVd, SrcType.no, srcStatus.srcType)   // ← 被改成 no
srcStatusNext.srcState  := ... | ignoreOldVd                                // 视为就绪
dataSources             := Mux(ignoreOldVd -> DataSource.imm, ...)          // ← 输出立即数 0
```

`NewDispatch.scala` 中有完全对称的一份（`ignoreOldVd`，并把 `srcType(numRegSrcVf-1)` 改成 `SrcType.no`）。

**RVV 语义**：当一条指令不依赖旧 vd（active 元素重写、tail 元素 agnostic），且 `vl=vlmax`、`vm=1`/`vta` 等条件成立时，没有任何元素需要保留旧 vd，于是硬件省略 old vd 的读取并喂 0。
- 我们的测试：`vsetvli e32,m1,ta,ma`，vl=4=vlmax（VLEN=128, SEW=32），`vm=1`、`vta=1` → `ignoreTail=true`。
- 乘加类指令**即使所有元素都 active，计算本身也要 old vd 参与累加**，因此必须声明 `isDependOldVd=true`。

`isDependOldVd` 在 `DecodeUnit.scala` 给出：

```scala
decodedInst.vpu.isDependOldVd :=
  isVppu || isVecOPF || isVStore || (isDstMask && !isOpMask) ||
  isNarrow || isVlx || isVma || isFof || vstartIsNotZero

val isVma = vmaInsts.map(_ === inst.ALL).reduce(_ || _)
```

而 `vmaInsts` 只列了**标准乘加**：

```scala
private val vmaInsts = Seq(
  VMACC_VV, VMACC_VX, VNMSAC_VV, VNMSAC_VX, VMADD_VV, VMADD_VX, VNMSUB_VV, VNMSUB_VX,
  VWMACCU_VV, VWMACCU_VX, VWMACC_VV, VWMACC_VX, VWMACCSU_VV, VWMACCSU_VX, VWMACCUS_VX,
)
```

**新增的 `VQDOT_VV / VQDOTU_VV / VQDOTSU_VV` 不在其中 → isVma=false → isDependOldVd=false → src2 被当成"不需要"而喂 0。**
标准 vmacc 走 isVma=true，所以不受影响。这与探针现象完全吻合。

---

## 3. 修复（一行列表）

`src/main/scala/xiangshan/backend/decode/DecodeUnit.scala`，`vmaInsts` 加入三条：

```scala
  private val vmaInsts = Seq(
    VMACC_VV, VMACC_VX, VNMSAC_VV, VNMSAC_VX, VMADD_VV, VMADD_VX, VNMSUB_VV, VNMSUB_VX,
    VWMACCU_VV, VWMACCU_VX, VWMACC_VV, VWMACC_VX, VWMACCSU_VV, VWMACCSU_VX, VWMACCUS_VX,
    // CIE2026 AsyncSys vdot：融合点积，vd = acc + Σ(vs1·vs2)，同属乘加、必须依赖 old vd
    VQDOT_VV, VQDOTU_VV, VQDOTSU_VV,
  )
```

修复后：vqdot 的 `isDependOldVd=true` → `ignoreOldVd=false` → src2 保留 `SrcType.vp`、走物理寄存器读/转发，old vd 正确进入运算核。

> 该修复属于"新指令在译码期的属性登记"，与运算 RTL 无关。**教训：给香山加一条新指令，光改译码表(BitPat/操作码)和执行单元是不够的，凡是按"指令类别"做的布尔判定（依赖 old vd、narrow、mask、reverse、wakeUp、写部分 vd 等）都要检查新指令是否需要登记。**

---

## 4. 验证

1. `mill -i xiangshan.compile` 全绿（860/860）。
2. 删 `build/rtl` 后 `build_simv.sh` 重 elaborate + VCS。
3. 跑 `run_vqdot.sh`：
   - VMACC-REF、DOT+OLD（期望 108/208/308/408）、OLDONLY（100/200/300/400）应全对；
   - 边界 ss/uu/su（old=0）通过；
   - 每类型 100 组随机（含非零 old）回归；
   - HIT GOOD TRAP、零 mismatch。
4. 保留了 Rename 输出探针 `[RN]`（op/st/l/p、srcType、lsrc、psrc、pdest）与 FU 探针，回归通过后可一并移除。

---

## 5. 涉及源码（服务器路径）

| 文件 | 作用 |
|---|---|
| `backend/decode/DecodeUnit.scala` | `vmaInsts`（976 行）、`isDependOldVd`（1069 行）—— **本次修复点** |
| `backend/issue/EntryBundles.scala` | src2 的 `ignoreOldVd` 改写（306–341 行） |
| `backend/dispatch/NewDispatch.scala` | 分派侧对称的 `ignoreOldVd`（268–330、453 行） |
| `backend/decode/VecDecoder.scala` | opmvv 表（383=VMACC，386–388=VQDOT） |
| `backend/rename/Rename.scala` | `psrc(2)=Mux1H(srcType(2)(2,1),…)`（393 行），含本次 `[RN]` 探针 |
| `backend/Bundles.scala` | `isDependOldVd` 字段（462 行）、15 列 decode 映射（129–137 行） |
| `backend/fu/wrapper/VIMacU.scala` | 例化 VDot64b、oldVd 选择 |
| `yunsuan/.../vectorIMAC/VDot64b.scala` | 点积运算核（已证明正确） |
