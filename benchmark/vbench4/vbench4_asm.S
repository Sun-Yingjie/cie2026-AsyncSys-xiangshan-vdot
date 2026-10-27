/*
 * CIE2026 AsyncSys — INT4 点积延迟对比（SEW=16）
 *
 * vqdot4：1 条自定义指令完成 8 个 16bit 累加器（每个累加 4 个 int4）。
 * base4：标准 RVV 实现——提取 4bit 子元素 -> 符号扩展到 16bit -> 有符号乘法 -> 向量累加。
 */

/* 提取 \S 中位置 \SH（0/4/8/12）的 4bit 子元素，符号扩展到 16bit，写入 \D；v1=0xf */
.macro EXTRACT4 D, S, SH
  .if \SH == 0
    vand.vv   \D, \S, v1
  .else
    vsrl.vi   \D, \S, \SH
    .if \SH < 12
      vand.vv \D, \D, v1
    .endif
  .endif
  vsll.vi     \D, \D, 12
  vsra.vi     \D, \D, 12
.endm

/* vqdot4：a0=A, a1=B, a2=old, a3=out */
.globl vqdot4
.type  vqdot4, @function
vqdot4:
  li    t1, 0x600; csrs mstatus, t1
  vsetvli t0, x0, e16, m1, ta, ma
  vle16.v v8, (a2)
  vle16.v v12, (a0)
  vle16.v v10, (a1)
  .word  0xB2002057 | (8 << 7) | (10 << 15) | (12 << 20)
  vse16.v v8, (a3)
  ret
.size vqdot4, .-vqdot4

/* base4：标准 RVV 基线；a0=A, a1=B, a2=old, a3=out */
.globl base4
.type  base4, @function
base4:
  li    t1, 0x600; csrs mstatus, t1
  vsetvli t0, x0, e16, m1, ta, ma
  li    t2, 0xf
  vmv.v.x v1, t2
  vle16.v v12, (a0)
  vle16.v v10, (a1)
  /* 提取 a: v20..v23 */
  EXTRACT4 v20, v12, 0
  EXTRACT4 v21, v12, 4
  EXTRACT4 v22, v12, 8
  EXTRACT4 v23, v12, 12
  /* 提取 b: v24..v27 */
  EXTRACT4 v24, v10, 0
  EXTRACT4 v25, v10, 4
  EXTRACT4 v26, v10, 8
  EXTRACT4 v27, v10, 12
  /* 乘积: v28..v31 */
  vmul.vv v28, v20, v24
  vmul.vv v29, v21, v25
  vmul.vv v30, v22, v26
  vmul.vv v31, v23, v27
  /* 累加 */
  vadd.vv v8, v28, v29
  vadd.vv v8, v8, v30
  vadd.vv v8, v8, v31
  vle16.v v9, (a2)
  vadd.vv v8, v8, v9
  vse16.v v8, (a3)
  ret
.size base4, .-base4
