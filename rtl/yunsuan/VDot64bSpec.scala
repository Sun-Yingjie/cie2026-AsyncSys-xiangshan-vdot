package yunsuan.vectortest.mac

import chiseltest._
import chisel3.util._
import org.scalatest.flatspec.AnyFlatSpec
import chisel3._
import yunsuan.vector.VIFuInfo
import yunsuan.vector.mac.VDot64b
import scala.util.Random

/* ============================ 测试 IO / Wrapper ============================ */

class VDot64bInput extends Bundle {
  // srcType(0)=vs2 srcType(1)=vs1, 4bit: bit[2]=sign, bit[1:0]=SEW编码(0=e8,1=e16)
  val srcType = Vec(2, UInt(4.W))
  val vs1 = UInt(64.W)
  val vs2 = UInt(64.W)
  val oldVd = UInt(64.W)
  val isMacc = Bool()
}
class VDot64bOutput extends Bundle {
  val vd = UInt(64.W) // 2 x int32
}

class VDot64bWrapper extends Module {
  val io = IO(new Bundle {
    val in = Flipped(Decoupled(new VDot64bInput))
    val out = Decoupled(new VDot64bOutput)
  })
  val dut = Module(new VDot64b)
  dut.io.fire := io.in.valid
  dut.io.info := 0.U.asTypeOf(new VIFuInfo)
  dut.io.srcType := io.in.bits.srcType
  dut.io.vdType := 0.U
  dut.io.vs1 := io.in.bits.vs1
  dut.io.vs2 := io.in.bits.vs2
  dut.io.oldVd := io.in.bits.oldVd
  dut.io.highHalf := false.B
  dut.io.isMacc := io.in.bits.isMacc
  dut.io.isSub := false.B
  dut.io.widen := true.B
  dut.io.isFixP := false.B

  io.out.bits.vd := dut.io.vd
  io.out.valid := RegNext(RegNext(io.in.valid))
  io.in.ready := io.out.ready
}

/* ============================ Scala 黄金参考 ============================ */
object VDotGolden {
  /** 取第 i 个元素并按需符号扩展。全程用 Long 移位：
   *  w=32 时 JVM Int 移位 1<<32 会回绕成 1<<0=1，导致符号修正多减 1。 */
  def elem(x: Long, i: Int, w: Int, signed: Boolean): Int = {
    val raw = (x >>> (i * w)) & ((1L << w) - 1) // 0..2^w-1
    (if (signed && (raw & (1L << (w - 1))) != 0L) raw - (1L << w) else raw).toInt
  }
  def pack2(i0: Int, i1: Int): Long =
    (i0.toLong & 0xffffffffL) | ((i1.toLong & 0xffffffffL) << 32)
  def dot(vs1: Long, vs2: Long, oldVd: Long, w: Int, s1: Boolean, s2: Boolean, macc: Boolean): Long = {
    val factor = 32 / w
    var out = Array(
      if (macc) elem(oldVd, 0, 32, true) else 0,
      if (macc) elem(oldVd, 1, 32, true) else 0
    )
    var grp = 0
    while (grp < 2) {
      var k = 0
      while (k < factor) {
        val i = grp * factor + k
        out(grp) = out(grp) + elem(vs1, i, w, s1) * elem(vs2, i, w, s2)
        k += 1
      }
      grp += 1
    }
    pack2(out(0), out(1))
  }
}

/* ============================ 测试 ============================ */

class VDot64bSpec extends AnyFlatSpec with ChiselScalatestTester {
  behavior of "VDot64b dot-product"

  // tag: 0=s8 1=u8 2=mixed8(vs2s,vs1u) 3=s16 4=u16
  case class Case(vs1: Long, vs2: Long, oldVd: Long, w: Int,
                  s1: Boolean, s2: Boolean, tVs2: Int, tVs1: Int,
                  macc: Boolean, tag: Int)

  def u64(x: Long): BigInt = (BigInt(x >>> 1) << 1) | (x & 1L)
  def bh(x: BigInt): String = "0x" + x.toString(16)
  def hx(x: Long): String = "0x" + x.toHexString

  def mkCase(rnd: Random, tag: Int): Case = {
    val vs1 = rnd.nextLong(); val vs2 = rnd.nextLong(); val old = rnd.nextLong()
    val macc = rnd.nextBoolean()
    tag match {
      case 0 => Case(vs1, vs2, old, 8, true, true, 4, 4, macc, tag)
      case 1 => Case(vs1, vs2, old, 8, false, false, 0, 0, macc, tag)
      case 2 => Case(vs1, vs2, old, 8, false, true, 4, 0, macc, tag)
      case 3 => Case(vs1, vs2, old, 16, true, true, 5, 5, macc, tag)
      case _ => Case(vs1, vs2, old, 16, false, false, 1, 1, macc, tag)
    }
  }
  val tagName = Map(0 -> "s8", 1 -> "u8", 2 -> "mixed8", 3 -> "s16", 4 -> "u16")

  /** 采样 1 拍 + 排空 2 拍，输出稳定后返回 vd。 */
  def runOne(dut: VDot64bWrapper, c: Case): BigInt = {
    dut.io.in.bits.vs1.poke(u64(c.vs1))
    dut.io.in.bits.vs2.poke(u64(c.vs2))
    dut.io.in.bits.oldVd.poke(u64(c.oldVd))
    dut.io.in.bits.srcType(0).poke(c.tVs2)
    dut.io.in.bits.srcType(1).poke(c.tVs1)
    dut.io.in.bits.isMacc.poke(c.macc)
    dut.io.in.valid.poke(true.B)
    dut.clock.step() // 采样
    dut.io.in.valid.poke(false.B)
    dut.clock.step(2) // 排空 2 拍，输出稳定
    dut.io.out.bits.vd.peekInt()
  }

  it should "pass directed int8 signed dot" in {
    test(new VDot64bWrapper) { dut =>
      dut.io.in.valid.poke(false.B)
      dut.io.out.ready.poke(true.B)
      val ones = 0x0101010101010101L
      def check(vs1: Long, vs2: Long, old: Long, macc: Boolean, exp: Long, note: String): Unit = {
        val got = runOne(dut, Case(vs1, vs2, old, 8, true, true, 4, 4, macc, 0))
        assert(got == u64(exp), s"$note: got=${bh(got)} exp=${bh(u64(exp))}")
      }
      check(ones, ones, 0L, false, 0x0000000400000004L, "no-macc")
      check(ones, ones, 0L, true, 0x0000000400000004L, "macc-old0")
      check(ones, ones, 0x0000000000000001L, true, 0x0000000400000005L, "macc-oldlo1")
      check(ones, ones, 0x0000000200000003L, true, 0x0000000600000007L, "macc-old23")
      check(ones, ones, 0xffffffffffffffffL, true, 0x0000000300000003L, "macc-oldff")
    }
  }

  for (tag <- 0 until 5) {
    it should s"pass random ${tagName(tag)}" in {
      test(new VDot64bWrapper) { dut =>
        dut.io.in.valid.poke(false.B)
        dut.io.out.ready.poke(true.B)
        val rnd = new Random(20261027 + tag)
        val N = 500
        var failMacc = 0; var failNoMacc = 0
        (0 until N).foreach { n =>
          val c = mkCase(rnd, tag)
          val got = runOne(dut, c)
          val exp = u64(VDotGolden.dot(c.vs1, c.vs2, c.oldVd, c.w, c.s1, c.s2, c.macc))
          if (got != exp) {
            if (c.macc) failMacc += 1 else failNoMacc += 1
            if (failMacc + failNoMacc <= 4) {
              println(s"[${tagName(tag)} #$n] macc=${c.macc} vs1=${hx(c.vs1)} vs2=${hx(c.vs2)} oldVd=${hx(c.oldVd)}")
              println(s"  got=${bh(got)} exp=${bh(exp)}")
            }
          }
        }
        assert(failMacc == 0 && failNoMacc == 0,
          s"${tagName(tag)} macc失败=$failMacc 非macc失败=$failNoMacc / $N")
      }
    }
  }
}
