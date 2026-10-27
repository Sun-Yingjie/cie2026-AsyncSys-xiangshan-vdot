package yunsuan.vectortest.mac

import chisel3._
import chiseltest._
import chisel3.util._
import org.scalatest.flatspec.AnyFlatSpec
import org.scalatest.matchers.should.Matchers
import yunsuan.vector.mac.VDot64b

/**
 * VDot64b 单元测试（对标 SiFive Xsfvqdotq，SEW=32 packed 8bit）。
 * 一个 64bit 模块：8 个 int8 -> 2 个 int32（每 4 个乘积归约）。
 * srcType 4bit: bit2=sign, bit1:0=sew；SEW=32 时 s32=6(0b0110)、u32=2(0b0010)。
 */
class VDot64bSpec extends AnyFlatSpec with ChiselScalatestTester with Matchers {

  val s32 = 6 // 0b0110 signed
  val u32 = 2 // 0b0010 unsigned

  case class VDot64bInput(
    vs1: Long, vs2: Long, oldVd: Long,
    vs2Sign: Boolean, vs1Sign: Boolean,
    isMacc: Boolean
  )
  case class VDot64bOutput(vd: Long, dotS2: Long, oldS2: Long, acc: Long, macc: Boolean)

  /* ============================ Golden 参考模型 ============================ */
  def elem8(x: Long, i: Int, signed: Boolean): Int = {
    val raw = ((x >>> (i * 8)) & 0xffL).toInt
    if (signed && (raw & 0x80) != 0) raw - 256 else raw
  }
  def elem32(x: Long, i: Int): Int = {
    val raw = (x >>> (i * 32)) & 0xffffffffL
    (if (raw >= 0x80000000L) raw - 0x100000000L else raw).toInt
  }
  def pack2(lo: Int, hi: Int): Long =
    (lo.toLong & 0xffffffffL) | ((hi.toLong & 0xffffffffL) << 32)

  def dotOnly(in: VDot64bInput): (Int, Int) = {
    val acc = new Array[Int](2)
    for (grp <- 0 until 2) {
      var s = 0
      for (k <- 0 until 4) {
        val i = grp * 4 + k
        s += elem8(in.vs1, i, in.vs1Sign) * elem8(in.vs2, i, in.vs2Sign)
      }
      acc(grp) = s
    }
    (acc(0), acc(1))
  }

  def runOne(in: VDot64bInput): VDot64bOutput = {
    val (d0, d1) = dotOnly(in)
    val o0 = elem32(in.oldVd, 0)
    val o1 = elem32(in.oldVd, 1)
    val a0 = if (in.isMacc) d0 + o0 else d0
    val a1 = if (in.isMacc) d1 + o1 else d1
    VDot64bOutput(pack2(a0, a1), pack2(d0, d1), in.oldVd, pack2(a0, a1), in.isMacc)
  }

  /* ============================ Wrapper ============================ */
  class VDot64bWrapper extends Module {
    val io = IO(new Bundle {
      val in = Flipped(Decoupled(new Bundle {
        val vs1 = UInt(64.W)
        val vs2 = UInt(64.W)
        val oldVd = UInt(64.W)
        val srcType = Vec(2, UInt(4.W))
        val isMacc = Bool()
      }))
      val out = Valid(new Bundle { val vd = UInt(64.W) })
      val dbgDotS2 = Output(UInt(64.W))
      val dbgOldS2 = Output(UInt(64.W))
      val dbgAcc = Output(UInt(64.W))
      val dbgMaccS2 = Output(Bool())
    })
    io.in.ready := true.B
    val dut = Module(new VDot64b)
    dut.io.fire := io.in.valid
    dut.io.info := 0.U.asTypeOf(dut.io.info)
    dut.io.srcType(0) := io.in.bits.srcType(0)
    dut.io.srcType(1) := io.in.bits.srcType(1)
    dut.io.vdType := s32.U
    dut.io.vs1 := io.in.bits.vs1
    dut.io.vs2 := io.in.bits.vs2
    dut.io.oldVd := io.in.bits.oldVd
    dut.io.highHalf := false.B
    dut.io.isMacc := io.in.bits.isMacc
    dut.io.isSub := false.B
    dut.io.widen := false.B
    dut.io.isFixP := false.B

    // VDot64b 内部已有 2 级寄存器（dotS1/dotS2）+ S2 组合输出；此处只对齐 2 拍 valid，
    // vd 直接组合引出（不能再套 Reg，否则比 valid 晚 2 拍）。
    io.out.valid := RegNext(RegNext(io.in.valid, false.B), false.B)
    io.out.bits.vd := dut.io.vd
    io.dbgDotS2 := dut.io.dbgDotS2
    io.dbgOldS2 := dut.io.dbgOldS2
    io.dbgAcc := dut.io.dbgAcc
    io.dbgMaccS2 := dut.io.dbgMaccS2
  }

  private val MASK64 = (BigInt(1) << 64) - 1
  def u64(x: Long): BigInt = BigInt(x) & MASK64

  val rng = new scala.util.Random(20261027)
  def rnd64(): Long = {
    val b = new Array[Byte](8); rng.nextBytes(b)
    java.nio.ByteBuffer.wrap(b).getLong
  }

  def feed(dut: VDot64bWrapper, in: VDot64bInput): Unit = {
    dut.io.in.valid.poke(true.B)
    dut.io.in.bits.srcType(0).poke((if (in.vs2Sign) s32 else u32).U)
    dut.io.in.bits.srcType(1).poke((if (in.vs1Sign) s32 else u32).U)
    dut.io.in.bits.isMacc.poke(in.isMacc.B)
    dut.io.in.bits.vs1.poke(u64(in.vs1).U)
    dut.io.in.bits.vs2.poke(u64(in.vs2).U)
    dut.io.in.bits.oldVd.poke(u64(in.oldVd).U)
    dut.clock.step(1)
    dut.io.in.valid.poke(false.B) // 只在 1 拍内打入，避免等待期间重复喂入
  }

  def h8(e: Seq[Int]): Long = {
    var x = 0L
    for (i <- 0 until 8) x |= ((e(i).toLong & 0xffL) << (i * 8))
    x
  }

  behavior of "VDot64b dot-product"

  it should "pass directed int8 signed dot" in {
    test(new VDot64bWrapper) { dut =>
      val ones = h8(Seq.fill(8)(1))
      feed(dut, VDot64bInput(ones, ones, 0L, true, true, false))
      val exp = runOne(VDot64bInput(ones, ones, 0L, true, true, false))
      while (!dut.io.out.valid.peekBoolean()) dut.clock.step(1)
      dut.io.out.bits.vd.peekInt().toLong shouldBe exp.vd

      val neg = h8(Seq.fill(8)(-1))
      val pos = h8(Seq.fill(8)(1))
      feed(dut, VDot64bInput(neg, pos, 0L, true, true, false))
      val exp2 = runOne(VDot64bInput(neg, pos, 0L, true, true, false))
      while (!dut.io.out.valid.peekBoolean()) dut.clock.step(1)
      dut.io.out.bits.vd.peekInt().toLong shouldBe exp2.vd

      feed(dut, VDot64bInput(ones, ones, pack2(1, 1), true, true, true))
      val exp3 = runOne(VDot64bInput(ones, ones, pack2(1, 1), true, true, true))
      while (!dut.io.out.valid.peekBoolean()) dut.clock.step(1)
      dut.io.out.bits.vd.peekInt().toLong shouldBe exp3.vd

      dut.io.in.valid.poke(false.B); dut.clock.step(3)
    }
  }

  def randomTest(name: String, vs2Sign: Boolean, vs1Sign: Boolean, n: Int): Unit = {
    it should s"pass random $name" in {
      test(new VDot64bWrapper) { dut =>
        var mismatchCount = 0
        for (_ <- 0 until n) {
          val in = VDot64bInput(rnd64(), rnd64(), rnd64(), vs2Sign, vs1Sign, rng.nextBoolean())
          val exp = runOne(in)
          feed(dut, in)
          while (!dut.io.out.valid.peekBoolean()) dut.clock.step(1)
          val got = dut.io.out.bits.vd.peekInt().toLong
          val dotS2 = dut.io.dbgDotS2.peekInt().toLong
          val oldS2 = dut.io.dbgOldS2.peekInt().toLong
          if (dotS2 != (exp.dotS2 & 0xffffffffffffffffL)) {
            println(s"[$name] dotS2 mismatch got=0x${dotS2.toHexString} exp=0x${exp.dotS2.toHexString}"); mismatchCount += 1
          }
          if (oldS2 != (exp.oldS2 & 0xffffffffffffffffL)) {
            println(s"[$name] oldS2 mismatch"); mismatchCount += 1
          }
          if (got != (exp.vd & 0xffffffffffffffffL)) {
            println(s"[$name] vd mismatch got=0x${got.toHexString} exp=0x${exp.vd.toHexString} " +
              s"dotS2=0x${dotS2.toHexString} oldS2=0x${oldS2.toHexString} macc=${exp.macc}")
            mismatchCount += 1
          }
        }
        dut.io.in.valid.poke(false.B); dut.clock.step(3)
        if (mismatchCount != 0) fail(s"$name had $mismatchCount mismatches")
      }
    }
  }

  randomTest("s32 (signed x signed)", true, true, 500)
  randomTest("u32 (unsigned x unsigned)", false, false, 500)
  randomTest("mixed (vs2 signed x vs1 unsigned)", true, false, 500)
}
