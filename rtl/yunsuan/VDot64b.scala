package yunsuan.vector.mac

import chisel3._
import chisel3.util._
import yunsuan.vector._
import yunsuan.util._

/**
 * 64-bit 向量点积（dot-product）数据模块 —— 香山 vqdot 自定义指令的核心运算核。
 *
 * 对标 SiFive Xsfvqdotq（vqdot）的 quad-widening 4D 点积语义，并实现 SiFive 提案
 * 第 4 页 NOTE 中"预留但未定义"的未来变体：
 *
 *   SEW=32（vdType=2）：每个 32bit 元素 packed 4 个 int8 子元素 -> 累加到 32bit（标准）
 *   SEW=16（vdType=1）：每个 16bit 元素 packed 4 个 int4 子元素 -> 累加到 16bit（INT4 扩展）
 *   SEW=64（vdType=3）：每个 64bit 元素 packed 4 个 int16 子元素 -> 累加到 64bit（INT16 扩展）
 *
 * 通用语义（每组 4 个子元素）：
 *   vd[i] = old_vd[i] + Σ_{k=0..3} sign_extend(vs1[i][k]) * sign_extend(vs2[i][k])
 *
 * 一个 64bit 模块：
 *   int8：2 组 × (4 个 int8)  -> 2 个 int32
 *   int4：4 组 × (4 个 int4)  -> 4 个 int16
 *   int16：1 组 × (4 个 int16) -> 1 个 int64
 *
 * 有符号/无符号仅由 srcType 的符号位（bit2）决定。
 *
 * 时序（与 VIMac64b 严格同构）：输入拍（fire）即在组合逻辑中把 oldVd 并入加法树，
 * 随后合并值只走两级流水寄存器：
 *   pre(S0 组合) -> s1(RegEnable, fire) -> s2(RegEnable, fireS1) -> io.vd(组合)
 *
 * 设计要点：
 *  - 乘积先归一到累加器位宽再做加法树，使全部加法在该位宽二补数下回绕（wrapping），
 *    不同累加器之间进位互不串扰。
 *  - 无符号元素「高位补 0」，有符号元素「复制符号位」。
 */
class VDot64b extends Module {
  val io = IO(new Bundle {
    val fire = Input(Bool())
    val info = Input(new VIFuInfo)
    val srcType = Input(Vec(2, UInt(4.W)))
    val vdType = Input(UInt(4.W))
    val vs1 = Input(UInt(64.W))
    val vs2 = Input(UInt(64.W))
    val oldVd = Input(UInt(64.W))
    val highHalf = Input(Bool())
    val isMacc = Input(Bool())
    val isSub = Input(Bool())
    val widen = Input(Bool())
    val isFixP = Input(Bool())
    val vd = Output(UInt(64.W))
    val vxsat = Output(Bool())
    // debug 观测点（S2 流水对齐，供 yunsuan 单测观测；集成进 VIMacU 时悬空即可）
    val dbgDotS2 = Output(UInt(64.W))
    val dbgOldS2 = Output(UInt(64.W))
    val dbgAcc = Output(UInt(64.W))
    val dbgMaccS2 = Output(Bool())
  })
  io.vxsat := false.B

  // 累加器 SEW 由 vdType 决定：SEW=32 -> int8；SEW=16 -> int4；SEW=64 -> int16。
  val sew = SewOH(io.vdType(1, 0))
  val isInt4 = sew.is16
  val isInt16 = sew.is64

  val vs2Sign = io.srcType(0)(2)
  val vs1Sign = io.srcType(1)(2)

  /** 归一到 SInt(n)：窄于 n 符号扩展，宽于 n 取低 n 位（二补数回绕）。 */
  def wrapn(x: SInt, n: Int): SInt = x.pad(n).asUInt(n - 1, 0).asSInt

  /** 取第 i 个 8bit 子元素，扩展为 SInt(9)。 */
  def elem8(x: UInt, i: Int, sign: Bool): SInt = {
    val raw = x(8 * i + 7, 8 * i)
    Mux(sign, Cat(raw(7), raw).asSInt, Cat(0.U(1.W), raw).asSInt)
  }

  /** 取第 i 个 4bit 子元素，扩展为 SInt(5)。 */
  def elem4(x: UInt, i: Int, sign: Bool): SInt = {
    val raw = x(4 * i + 3, 4 * i)
    Mux(sign, Cat(raw(3), raw).asSInt, Cat(0.U(1.W), raw).asSInt)
  }

  /** 取第 i 个 16bit 子元素，扩展为 SInt(17)。 */
  def elem16(x: UInt, i: Int, sign: Bool): SInt = {
    val raw = x(16 * i + 15, 16 * i)
    Mux(sign, Cat(raw(15), raw).asSInt, Cat(0.U(1.W), raw).asSInt)
  }

  /* ---------- int8：2 组，每组 4 个 int8 -> int32 ---------- */
  val pre8 = Wire(Vec(2, SInt(32.W)))
  val dot8 = Wire(Vec(2, UInt(32.W)))
  val old8 = Wire(Vec(2, UInt(32.W)))
  for (grp <- 0 until 2) {
    val prods = Seq.tabulate(4) { k =>
      val i = grp * 4 + k
      wrapn(elem8(io.vs1, i, vs1Sign) * elem8(io.vs2, i, vs2Sign), 32)
    }
    val dot = wrapn(prods(0) + prods(1) + prods(2) + prods(3), 32)
    val old = io.oldVd(32 * grp + 31, 32 * grp).asSInt
    pre8(grp) := wrapn(Mux(io.isMacc, dot + old, dot), 32)
    dot8(grp) := dot.asUInt
    old8(grp) := io.oldVd(32 * grp + 31, 32 * grp)
  }

  /* ---------- int4：4 组，每组 4 个 int4 -> int16 ---------- */
  val pre4 = Wire(Vec(4, SInt(16.W)))
  val dot4 = Wire(Vec(4, UInt(16.W)))
  val old4 = Wire(Vec(4, UInt(16.W)))
  for (grp <- 0 until 4) {
    val prods = Seq.tabulate(4) { k =>
      val i = grp * 4 + k
      wrapn(elem4(io.vs1, i, vs1Sign) * elem4(io.vs2, i, vs2Sign), 16)
    }
    val dot = wrapn(prods(0) + prods(1) + prods(2) + prods(3), 16)
    val old = io.oldVd(16 * grp + 15, 16 * grp).asSInt
    pre4(grp) := wrapn(Mux(io.isMacc, dot + old, dot), 16)
    dot4(grp) := dot.asUInt
    old4(grp) := io.oldVd(16 * grp + 15, 16 * grp)
  }

  /* ---------- int16：1 组，4 个 int16 -> int64 ---------- */
  // 乘积（16bit×16bit=32bit）先归一到 64bit 再累加，避免 32bit 中间和溢出
  val prods16 = Seq.tabulate(4) { k =>
    wrapn(elem16(io.vs1, k, vs1Sign) * elem16(io.vs2, k, vs2Sign), 64)
  }
  val dot16 = wrapn(prods16(0) + prods16(1) + prods16(2) + prods16(3), 64)
  val pre16w: SInt = wrapn(Mux(io.isMacc, dot16 + io.oldVd.asSInt, dot16), 64)
  val dot16w: UInt = dot16.asUInt
  val old16w: UInt = io.oldVd

  /* ---------- 按 SEW 选择并打包成 64bit ---------- */
  val pre8Bits = Cat(pre8(1).asUInt, pre8(0).asUInt)
  val pre4Bits = Cat(pre4(3).asUInt, pre4(2).asUInt, pre4(1).asUInt, pre4(0).asUInt)
  val pre16Bits = pre16w.asUInt
  val dot8Bits = Cat(dot8(1), dot8(0))
  val dot4Bits = Cat(dot4(3), dot4(2), dot4(1), dot4(0))
  val dot16Bits = dot16w
  val old8Bits = Cat(old8(1), old8(0))
  val old4Bits = Cat(old4(3), old4(2), old4(1), old4(0))
  val old16Bits = old16w
  val pre = Mux(isInt16, pre16Bits, Mux(isInt4, pre4Bits, pre8Bits))
  val dotBits = Mux(isInt16, dot16Bits, Mux(isInt4, dot4Bits, dot8Bits))
  val oldBits = Mux(isInt16, old16Bits, Mux(isInt4, old4Bits, old8Bits))

  /* ---------- 两级流水（与 VIMac64b 的 wallaceOut_mid_reg / walOut 对齐） ---------- */
  val fireS1 = GatedValidRegNext(io.fire)
  val s1 = RegEnable(pre, io.fire)
  val s2 = RegEnable(s1, fireS1)
  val dotS2 = RegEnable(RegEnable(dotBits, io.fire), fireS1)
  val oldS2 = RegEnable(RegEnable(oldBits, io.fire), fireS1)
  val maccS2 = RegEnable(RegEnable(io.isMacc, io.fire), fireS1)

  io.vd := s2
  io.dbgDotS2 := dotS2
  io.dbgOldS2 := oldS2
  io.dbgAcc := s2
  io.dbgMaccS2 := maccS2
}
