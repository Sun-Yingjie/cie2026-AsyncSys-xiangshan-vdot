/*
 * CIE2026 AsyncSys — INT4 vqdot 测试汇编（SEW=16，SiFive 预留的未来变体）
 *
 * 每个 16bit 元素 packed 4 个 int4 子元素；一条 vqdot（SEW=16）：
 *   vd[lane](16b) = old_vd[lane] + Σ_{k=0..3} sext(vs1[lane][k](4b) * vs2[lane][k](4b))
 * 128bit = 8 个 16bit = 32 个 int4。
 *
 * 指令字与 int8 版本相同（0xB2002057 等），硬件按 vtype 的 SEW 选择 int4/int8。
 */

/* 通用：a0=vs2(A), a1=vs1(B), a2=old(vd), a3=out；指令字在 a4 传入 */
.macro DOT4_BODY OPCODE
  li    t1, 0x600
  csrs  mstatus, t1
  vsetvli t0, x0, e16, m1, ta, ma
  vle16.v v8, (a2)
  vle16.v v12, (a0)
  vle16.v v10, (a1)
  .word  \OPCODE | (8 << 7) | (10 << 15) | (12 << 20)
  vse16.v v8, (a3)
  ret
.endm

/* signed × signed */
.globl dot4_ss
.type  dot4_ss, @function
dot4_ss:
  DOT4_BODY 0xB2002057
.size dot4_ss, .-dot4_ss

/* unsigned × unsigned */
.globl dot4_uu
.type  dot4_uu, @function
dot4_uu:
  DOT4_BODY 0xA2002057
.size dot4_uu, .-dot4_uu

/* vs2 signed × vs1 unsigned */
.globl dot4_su
.type  dot4_su, @function
dot4_su:
  DOT4_BODY 0xAA002057
.size dot4_su, .-dot4_su
