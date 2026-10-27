/*
 * CIE2026 AsyncSys — INT16 vqdot 测试汇编（SEW=64，SiFive 映射的最高精度变体）
 *
 * 每个 64bit 元素 packed 4 个 int16 子元素；一条 vqdot（SEW=64）：
 *   vd[lane](64b) = old_vd[lane] + Σ_{k=0..3} sext(vs1[lane][k](16b) * vs2[lane][k](16b))
 * 128bit = 2 个 64bit = 8 个 int16。
 *
 * 指令字与 int8/int4 版本相同（0xB2002057 等），硬件按 vtype 的 SEW 选择 int16/int8/int4。
 */

/* 通用：a0=vs2(A), a1=vs1(B), a2=old(vd), a3=out；指令字在 a4 传入 */
.macro DOT16_BODY OPCODE
  li    t1, 0x600
  csrs  mstatus, t1
  vsetvli t0, x0, e64, m1, ta, ma
  vle64.v v8, (a2)
  vle64.v v12, (a0)
  vle64.v v10, (a1)
  .word  \OPCODE | (8 << 7) | (10 << 15) | (12 << 20)
  vse64.v v8, (a3)
  ret
.endm

/* signed × signed */
.globl dot16_ss
.type  dot16_ss, @function
dot16_ss:
  DOT16_BODY 0xB2002057
.size dot16_ss, .-dot16_ss

/* unsigned × unsigned */
.globl dot16_uu
.type  dot16_uu, @function
dot16_uu:
  DOT16_BODY 0xA2002057
.size dot16_uu, .-dot16_uu

/* vs2 signed × vs1 unsigned */
.globl dot16_su
.type  dot16_su, @function
dot16_su:
  DOT16_BODY 0xAA002057
.size dot16_su, .-dot16_su
