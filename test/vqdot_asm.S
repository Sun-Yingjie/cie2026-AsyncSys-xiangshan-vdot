/*
 * CIE2026 AsyncSys — vqdot 指令端到端测试（对标 SiFive Xsfvqdotq，OP-V/OPMVV）
 *
 * 指令编码（SEW=32，vm=1，funct3=010=OPMVV，opcode=0x57）：
 *   funct6(31:26) | vm(25) | vs2(24:20) | vs1(19:15) | 010(14:12) | vd(11:7) | 1010111
 *   vqdot.vv    (SS)            funct6=101100 -> base 0xB2002057
 *   vqdotu.vv   (UU)            funct6=101000 -> base 0xA2002057
 *   vqdotsu.vv  (vs2 S x vs1 U) funct6=101010 -> base 0xAA002057
 *
 * 宏参数为寄存器编号（vd, vs2, vs1），均为数字，便于直接拼装 .word。
 */
.section .text
.option norvc

.macro VQDOT_VV vd, vs2, vs1
  .word (0xB2002057 + ((\vd) << 7) + ((\vs1) << 15) + ((\vs2) << 20))
.endm

.macro VQDOTU_VV vd, vs2, vs1
  .word (0xA2002057 + ((\vd) << 7) + ((\vs1) << 15) + ((\vs2) << 20))
.endm

.macro VQDOTSU_VV vd, vs2, vs1
  .word (0xAA002057 + ((\vd) << 7) + ((\vs1) << 15) + ((\vs2) << 20))
.endm

# void vqdot_run(a0=vs1, a1=vs2, a2=oldvd, a3=out, a4=type)
# 寄存器约定：v8 = vd/old（累加目标），v9 = vs1，v10 = vs2。
# SEW=32、LMUL=1 时 vl = VLEN/32 = 4（VLEN=128），即每个向量寄存器 4 个 32bit lane。
.globl vqdot_run
.type vqdot_run, @function
vqdot_run:
  # nexus-am 启动代码只使能了 mstatus.FS，这里补使能 mstatus.VS=Dirty（bits[10:9]=11）
  li    t1, 0x600
  csrs  mstatus, t1
  vsetvli t0, x0, e32, m1, ta, ma   /* SEW=32, LMUL=1, ta=1, ma=1；vl=4 写入 t0 */
  vle32.v v8, (a2)                   /* v8  = old vd（本测试取 0） */
  vle32.v v9, (a0)                   /* v9  = vs1 */
  vle32.v v10, (a1)                  /* v10 = vs2 */

  li    t0, 0
  beq   a4, t0, .Lss
  li    t0, 1
  beq   a4, t0, .Luu
  li    t0, 3
  beq   a4, t0, .Lvmacc
  j     .Lsu

.Lss:
  VQDOT_VV   8, 10, 9                /* v8.vqdot(v10, v9)   signed x signed */
  j     .Lstore
.Luu:
  VQDOTU_VV  8, 10, 9                /* v8.vqdotu(v10, v9)  unsigned x unsigned */
  j     .Lstore
.Lvmacc:
  vmacc.vv   v8, v9, v10             /* 标准累加对照：v8 = v8 + v9*v10（SEW32） */
  j     .Lstore
.Lsu:
  VQDOTSU_VV 8, 10, 9                /* v8.vqdotsu(v10, v9) vs2 signed x vs1 unsigned */

.Lstore:
  vse32.v v8, (a3)                   /* 回写累加结果 */
  ret
.size vqdot_run, .-vqdot_run
