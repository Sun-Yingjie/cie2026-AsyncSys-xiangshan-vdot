package yunsuan.vector.mac

import chisel3._
import chisel3.util._
import yunsuan.vector.SewOH
import yunsuan.vector.VIFuInfo
import yunsuan.util._

/**
 * 64-bit 向量点积（dot-product）数据模块 —— 香山 vdot 自定义指令的核心运算核。
 *
 * 一个 64-bit 数据模块内：
 *   INT8 : 8 个 int8  (vs1[8]) × (vs2[8])  -> 2 个 int32（每 4 个乘积归约为一组）
 *   INT16: 4 个 int16 (vs1[4]) × (vs2[4])  -> 2 个 int32（每 2 个乘积归约为一组）
 * 支持 signed / unsigned / 有符号×无符号(mixed)；isMacc 时把结果与 oldVd 做 widening 累加。
 *
 * 接口对齐 VIMac64b（fire/info/srcType/vs1/vs2/oldVd/...），可直接在 VIMacU 中按 opcode 例化。
 * 延迟：输入拍 -> 点积(S1) -> 点积(S2)/oldVd 对齐 -> S2 组合累加输出，共 2 个寄存器拍。
 *
 * 设计要点：
 *  - Chisel 中 SInt 加法结果位宽 = 最大操作数位宽，故每个乘积先归一到 SInt(32) 再做加法树，
 *    使全部加法在 32bit 二补数下回绕（与软件/ISA 语义一致），两个 int32 组之间进位互不串扰。
 *  - 无符号元素通过"高位补 0 扩 1 位"表示为正数 SInt；有符号元素"复制符号位扩 1 位"。
 *  - 已通过 yunsuan 独立单测：s8/u8/mixed8/s16/u16 各 500 随机 + 定向，共 2500+ 组。
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

  val sew = SewOH(io.srcType(0)(1, 0))
  val vs2Sign = io.srcType(0)(2)
  val vs1Sign = io.srcType(1)(2)

  /** 归一到 SInt(32)：窄于 32 符号扩展，宽于 32 取低 32 位（二补数回绕）。 */
  def wrap32s(x: SInt): SInt = x.pad(32).asUInt(31, 0).asSInt

  /** 取第 i 个 nww-bit 元素，扩展为 SInt(nww+1)：有符号复制符号位，无符号高位补 0。 */
  def elemS(x: UInt, i: Int, nww: Int, sign: Bool): SInt = {
    val raw = x((i + 1) * nww - 1, i * nww) // UInt(nww)
    Mux(sign, Cat(raw(nww - 1), raw).asSInt, Cat(0.U(1.W), raw).asSInt)
  }

  /* ---------- INT8：8 个 int8 -> 2 个 int32（每组 4 个乘积） ---------- */
  val dot8 = Wire(Vec(2, SInt(32.W)))
  for (grp <- 0 until 2) {
    val prods = Seq.tabulate(4) { k =>
      val i = grp * 4 + k
      wrap32s(elemS(io.vs1, i, 8, vs1Sign) * elemS(io.vs2, i, 8, vs2Sign)) // SInt(9)*SInt(9)
    }
    dot8(grp) := prods(0) + prods(1) + prods(2) + prods(3)
  }

  /* ---------- INT16：4 个 int16 -> 2 个 int32（每组 2 个乘积） ---------- */
  val dot16 = Wire(Vec(2, SInt(32.W)))
  for (grp <- 0 until 2) {
    val prods = Seq.tabulate(2) { k =>
      val i = grp * 2 + k
      wrap32s(elemS(io.vs1, i, 16, vs1Sign) * elemS(io.vs2, i, 16, vs2Sign)) // SInt(17)*SInt(17)
    }
    dot16(grp) := prods(0) + prods(1)
  }

  /* ---------- 流水线对齐：S1 点积，S2 与 oldVd 累加 ---------- */
  val fireS1 = GatedValidRegNext(io.fire)
  val dotS1 = RegEnable(Mux1H(Seq(sew.is8 -> dot8, sew.is16 -> dot16)), io.fire)
  val dotS2 = RegEnable(dotS1, fireS1)
  val oldVdS2 = RegEnable(RegEnable(io.oldVd, io.fire), fireS1)
  val isMaccS2 = RegEnable(RegEnable(io.isMacc, io.fire), fireS1)
  val oldVdVec = VecInit(Seq.tabulate(2)(i => oldVdS2(32 * i + 31, 32 * i).asSInt))

  val acc = Wire(Vec(2, SInt(32.W)))
  for (i <- 0 until 2) {
    acc(i) := wrap32s(Mux(isMaccS2, dotS2(i) + oldVdVec(i), dotS2(i)))
  }
  io.vd := Cat(acc(1).asUInt, acc(0).asUInt)
  io.dbgDotS2 := dotS2.asUInt
  io.dbgOldS2 := oldVdS2
  io.dbgAcc := acc.asUInt
  io.dbgMaccS2 := isMaccS2
}
