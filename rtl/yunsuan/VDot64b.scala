package yunsuan.vector.mac

import chisel3._
import chisel3.util._
import yunsuan.vector.VIFuInfo
import yunsuan.util._

/**
 * 64-bit 向量点积（dot-product）数据模块 —— 香山 vdot 自定义指令的核心运算核。
 *
 * 对标 SiFive Xsfvqdotq（vqdot）的 8-bit -> 32-bit 点积语义：
 *   指令在 SEW=32 下执行；每个 32bit 元素的 vs1/vs2 被重新解释为 4 个 packed 8bit
 *   子元素，子元素两两相乘、符号扩展到 32bit，再做 4 项归约并累加到对应 32bit vd：
 *       vd[i](32b) = old_vd[i] + Σ_{k=0..3} sign_extend(vs1[i*4+k] * vs2[i*4+k])
 *
 * 一个 64bit 模块 = 2 个 32bit 元素 = 8 个 int8  -> 2 个 int32（每组 4 个乘积归约）。
 * 子元素宽度固定 8bit（不随 vsew 变化，SEW!=32 由译码判 illegal）；
 * 有符号/无符号仅由 srcType 的 sign 位（bit2）决定，与 sew 字段无关。
 *
 * 时序（与 VIMac64b 严格同构，避免独立的旧值寄存器链在真实流水反压下错位）：
 *   - 输入拍（fire）即在组合逻辑中把 oldVd 并入加法树（VIMac64b 也是在 S0 把 oldVd
 *     注入 partProd），随后合并值只走两级流水寄存器：
 *       pre(S0 组合) -> s1(RegEnable, fire) -> s2(RegEnable, fireS1) -> io.vd(组合)
 *   - 这样 oldVd 与乘积共享完全相同的使能与寄存器，天然对齐，共 2 个寄存器拍。
 *
 * 设计要点：
 *  - Chisel 中 SInt 加法结果位宽 = 最大操作数位宽，故每个乘积先归一到 SInt(32) 再做加法树，
 *    使全部加法在 32bit 二补数下回绕（与 SiFive 规约的 wrapping on overflow 一致），
 *    两个 int32 累加器之间进位互不串扰。
 *  - 无符号元素「高位补 0 扩 1 位」表示为正 SInt；有符号元素「复制符号位扩 1 位」。
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

  // 仅取符号位；子元素宽度固定 8bit，忽略 srcType 的 sew 字段（SEW 恒为 32）。
  val vs2Sign = io.srcType(0)(2)
  val vs1Sign = io.srcType(1)(2)

  /** 归一到 SInt(32)：窄于 32 符号扩展，宽于 32 取低 32 位（二补数回绕）。 */
  def wrap32s(x: SInt): SInt = x.pad(32).asUInt(31, 0).asSInt

  /** 取第 i 个 8bit 子元素，扩展为 SInt(9)：有符号复制符号位，无符号高位补 0。 */
  def elemS(x: UInt, i: Int, sign: Bool): SInt = {
    val raw = x(8 * i + 7, 8 * i) // UInt(8)
    Mux(sign, Cat(raw(7), raw).asSInt, Cat(0.U(1.W), raw).asSInt)
  }

  /*
   * S0（fire 拍）组合：每 lane 4 个 int8 乘积归约，并在输入拍合并 oldVd。
   * oldVd 与乘积在进入第一级寄存器前就合并（对齐 VIMac64b 的 partProd 旧值注入）。
   */
  val pre = Wire(Vec(2, SInt(32.W)))
  val dotW = Wire(Vec(2, UInt(32.W)))
  val oldW = Wire(Vec(2, UInt(32.W)))
  for (grp <- 0 until 2) {
    val prods = Seq.tabulate(4) { k =>
      val i = grp * 4 + k
      wrap32s(elemS(io.vs1, i, vs1Sign) * elemS(io.vs2, i, vs2Sign)) // SInt(9)*SInt(9)
    }
    val dot = wrap32s(prods(0) + prods(1) + prods(2) + prods(3))
    val old = io.oldVd(32 * grp + 31, 32 * grp).asSInt
    pre(grp) := wrap32s(Mux(io.isMacc, dot + old, dot))
    dotW(grp) := dot.asUInt
    oldW(grp) := io.oldVd(32 * grp + 31, 32 * grp)
  }

  /* ---------- 两级流水（与 VIMac64b 的 wallaceOut_mid_reg / walOut 对齐） ---------- */
  val fireS1 = GatedValidRegNext(io.fire)
  val s1 = RegEnable(pre, io.fire)
  val s2 = RegEnable(s1, fireS1)
  // 仅供 debug 观测：点积与旧值随数据同步寄存两级。
  val dotS2 = RegEnable(RegEnable(dotW, io.fire), fireS1)
  val oldS2 = RegEnable(RegEnable(oldW, io.fire), fireS1)
  val maccS2 = RegEnable(RegEnable(io.isMacc, io.fire), fireS1)

  io.vd := Cat(s2(1).asUInt, s2(0).asUInt)
  io.dbgDotS2 := dotS2.asUInt
  io.dbgOldS2 := oldS2.asUInt
  io.dbgAcc := s2.asUInt
  io.dbgMaccS2 := maccS2
}
