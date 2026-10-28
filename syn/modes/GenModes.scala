package yunsuan.vector.mac

import java.io.{File, PrintWriter}

/** 生成三档单档 SV（用于分档面积评估）：mill runMain yunsuan.vector.mac.GenModes */
object GenModes extends App {
  val outdir = new File("syn_modes_out")
  outdir.mkdirs()
  val modes = Seq(("int8", 0), ("int4", 1), ("int16", 2))
  for ((name, mode) <- modes) {
    val sv = circt.stage.ChiselStage.emitSystemVerilog(new VDot64b(onlyMode = mode))
    val pw = new PrintWriter(new File(outdir, s"VDot_$name.sv"))
    pw.write(sv)
    pw.close()
    println(s"generated VDot_$name.sv")
  }
}
