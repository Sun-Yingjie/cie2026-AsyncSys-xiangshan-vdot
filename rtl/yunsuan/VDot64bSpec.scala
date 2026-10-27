package yunsuan.vectortest.mac

import chisel3._
import chiseltest._
import chisel3.util._
import org.scalatest.flatspec.AnyFlatSpec
import org.scalatest.matchers.should.Matchers
import yunsuan.vector.mac.VDot64b

/**
 * VDot64b 单元测试，覆盖两种 SEW（SiFive 提案变体）：
 *   SEW=32：2 组 × 4 个 int8  -> 2 个 int32（标准）
 *   SEW=16：4 组 × 4 个 int4  -> 4 个 int16（INT4 扩展）
 * srcType/vdType 4bit: bit2=sign, bit1:0=sew
 *   s32=6(0110) u32=2(0010) s16=5(0101) u16=1(0001)
 */
class VDot64bSpec extends AnyFlatSpec with ChiselScalatestTester with Matchers {

  val s32 = 6
  val u32 = 2
  val s16 = 5
  val u16 = 1

  case class VDotInput(
    vs1: Long, vs2: Long, oldVd: Long,
    vs2Sign: Boolean, vs1Sign: Boolean,
    isMacc: Boolean, isInt4: Boolean
  )

  /* ============================ Golden 参考模型 ============================ */
  def elemN(x: Long, i: Int, bits: Int, signed: Boolean): Int = {
    val mask = (1L << bits) - 1
    val raw = ((x >>> (i * bits)) & mask).toInt
    val signBit = 1 << (bits - 1)
    if (signed && (raw & signBit) != 0) raw - (1 << bits) else raw
  }

  def runOne(in: VDotInput): Long = {
    if (in.isInt4) {
      // 4 组 × 4 个 int4 -> 4 个 int16
      var out = 0L
      for (grp <- 0 until 4) {
        var s = 0
        for (k <- 0 until 4) {
          val i = grp * 4 + k
          s += elemN(in.vs1, i, 4, in.vs1Sign) * elemN(in.vs2, i, 4, in.vs2Sign)
        }
        val old = ((in.oldVd >>> (grp * 16)) & 0xffffL).toInt
        val r = (if (in.isMacc) s + old else s) & 0xffff
        out |= (r.toLong & 0xffffL) << (grp * 16)
      }
      out
    } else {
      // 2 组 × 4 个 int8 -> 2 个 int32
      var out = 0L
      for (grp <- 0 until 2) {
        var s = 0
        for (k <- 0 until 4) {
          val i = grp * 4 + k
          s += elemN(in.vs1, i, 8, in.vs1Sign) * elemN(in.vs2, i, 8, in.vs2Sign)
        }
        val oldRaw = (in.oldVd >>> (grp * 32)) & 0xffffffffL
        val old = if (oldRaw >= 0x80000000L) (oldRaw - 0x100000000L).toInt else oldRaw.toInt
        val r = if (in.isMacc) s + old else s
        out |= (r.toLong & 0xffffffffL) << (grp * 32)
      }
      out
    }
  }

  /* ============================ Wrapper ============================ */
  class VDotWrapper extends Module {
    val io = IO(new Bundle {
      val in = Flipped(Decoupled(new Bundle {
        val vs1 = UInt(64.W)
        val vs2 = UInt(64.W)
        val oldVd = UInt(64.W)
        val srcType = Vec(2, UInt(4.W))
        val vdType = UInt(4.W)
        val isMacc = Bool()
      }))
      val out = Valid(new Bundle { val vd = UInt(64.W) })
    })
    io.in.ready := true.B
    val dut = Module(new VDot64b)
    dut.io.fire := io.in.valid
    dut.io.info := 0.U.asTypeOf(dut.io.info)
    dut.io.srcType(0) := io.in.bits.srcType(0)
    dut.io.srcType(1) := io.in.bits.srcType(1)
    dut.io.vdType := io.in.bits.vdType
    dut.io.vs1 := io.in.bits.vs1
    dut.io.vs2 := io.in.bits.vs2
    dut.io.oldVd := io.in.bits.oldVd
    dut.io.highHalf := false.B
    dut.io.isMacc := io.in.bits.isMacc
    dut.io.isSub := false.B
    dut.io.widen := false.B
    dut.io.isFixP := false.B
    io.out.valid := RegNext(RegNext(io.in.valid, false.B), false.B)
    io.out.bits.vd := dut.io.vd
  }

  private val MASK64 = (BigInt(1) << 64) - 1
  def u64(x: Long): BigInt = BigInt(x) & MASK64

  val rng = new scala.util.Random(20261027)
  def rnd64(): Long = {
    val b = new Array[Byte](8); rng.nextBytes(b)
    java.nio.ByteBuffer.wrap(b).getLong
  }

  def feed(dut: VDotWrapper, in: VDotInput): Unit = {
    val (st2, st1, vdt) =
      if (in.isInt4) (s16, s16, s16.U) else (s32, s32, s32.U)
    dut.io.in.valid.poke(true.B)
    dut.io.in.bits.srcType(0).poke((if (in.vs2Sign) st2 else (if (in.isInt4) u16 else u32)).U)
    dut.io.in.bits.srcType(1).poke((if (in.vs1Sign) st1 else (if (in.isInt4) u16 else u32)).U)
    dut.io.in.bits.vdType.poke(vdt)
    dut.io.in.bits.isMacc.poke(in.isMacc.B)
    dut.io.in.bits.vs1.poke(u64(in.vs1).U)
    dut.io.in.bits.vs2.poke(u64(in.vs2).U)
    dut.io.in.bits.oldVd.poke(u64(in.oldVd).U)
    dut.clock.step(1)
    dut.io.in.valid.poke(false.B)
  }

  def randomTest(name: String, isInt4: Boolean, vs2Sign: Boolean, vs1Sign: Boolean, n: Int): Unit = {
    it should s"pass $name" in {
      test(new VDotWrapper) { dut =>
        var mismatchCount = 0
        for (_ <- 0 until n) {
          val in = VDotInput(rnd64(), rnd64(), rnd64(), vs2Sign, vs1Sign, rng.nextBoolean(), isInt4)
          val exp = runOne(in)
          feed(dut, in)
          while (!dut.io.out.valid.peekBoolean()) dut.clock.step(1)
          val got = dut.io.out.bits.vd.peekInt().toLong
          if (got != (exp & MASK64.toLong)) {
            println(s"[$name] mismatch got=0x${got.toHexString} exp=0x${exp.toHexString}"); mismatchCount += 1
          }
        }
        dut.io.in.valid.poke(false.B); dut.clock.step(3)
        if (mismatchCount != 0) fail(s"$name had $mismatchCount mismatches")
      }
    }
  }

  it should "pass directed int4 signed dot" in {
    test(new VDotWrapper) { dut =>
      // 16 个 int4 全为 1：每组 4 个，dot=4；4 组
      var ones = 0L
      for (i <- 0 until 16) ones |= (1L << (i * 4))
      feed(dut, VDotInput(ones, ones, 0L, true, true, false, true))
      val exp = runOne(VDotInput(ones, ones, 0L, true, true, false, true))
      while (!dut.io.out.valid.peekBoolean()) dut.clock.step(1)
      dut.io.out.bits.vd.peekInt().toLong shouldBe exp
      // 含 old
      var old = 0L
      for (g <- 0 until 4) old |= (10L << (g * 16))
      feed(dut, VDotInput(ones, ones, old, true, true, true, true))
      val exp2 = runOne(VDotInput(ones, ones, old, true, true, true, true))
      while (!dut.io.out.valid.peekBoolean()) dut.clock.step(1)
      dut.io.out.bits.vd.peekInt().toLong shouldBe exp2
      dut.io.in.valid.poke(false.B); dut.clock.step(3)
    }
  }

  // INT8（标准）
  randomTest("int8 s32 (signed x signed)", false, true, true, 300)
  randomTest("int8 u32 (unsigned x unsigned)", false, false, false, 300)
  randomTest("int8 mixed (vs2 signed x vs1 unsigned)", false, true, false, 300)
  // INT4（扩展）
  randomTest("int4 s16 (signed x signed)", true, true, true, 300)
  randomTest("int4 u16 (unsigned x unsigned)", true, false, false, 300)
  randomTest("int4 mixed (vs2 signed x vs1 unsigned)", true, true, false, 300)
}
