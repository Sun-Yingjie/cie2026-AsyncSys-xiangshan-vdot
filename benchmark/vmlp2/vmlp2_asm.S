/*
 * CIE2026 AsyncSys — 混合精度量化 MLP（INT4 层 + INT8 层）
 *
 *  vqdot 路径：
 *   gemv8_int4  (a0=W1p,a1=xp,a2=b1,a3=h1)        M=8,N=16
 *   gemv4_int8  (a0=W2p,a1=h8,a2=b2,a3=y,a4=N)    M=4
 *  标准 RVV baseline：
 *   gemv8_int4_base / gemv4_int8_base（同接口）
 */
.section .text
.option norvc

.macro VQDOT vd, vs2, vs1
  .word (0xB2002057 + ((\vd) << 7) + ((\vs1) << 15) + ((\vs2) << 20))
.endm

/* ===== 第一层 INT4 vqdot ===== */
.globl gemv8_int4
.type  gemv8_int4, @function
gemv8_int4:
  li    t1, 0x600; csrs mstatus, t1
  vsetvli t0, x0, e16, m1, ta, ma
  vle16.v v8, (a2)
  li    t2, 0
1:
  vle16.v v12, (a0)
  add   t4, a1, t2
  lh    t5, (t4)
  vmv.v.x v10, t5
  VQDOT 8, 12, 10
  addi  a0, a0, 16
  addi  t2, t2, 2
  li    t6, 8
  blt   t2, t6, 1b
  vse16.v v8, (a3)
  ret
.size gemv8_int4, .-gemv8_int4

/* ===== 第二层 INT8 vqdot ===== */
.globl gemv4_int8
.type  gemv4_int8, @function
gemv4_int8:
  li    t1, 0x600; csrs mstatus, t1
  vsetvli t0, x0, e32, m1, ta, ma
  vle32.v v8, (a2)
  mv    t2, x0
  mv    t6, a4
1:
  vle32.v v12, (a0)
  add   t4, a1, t2
  lw    t5, (t4)
  vmv.v.x v10, t5
  VQDOT 8, 12, 10
  addi  a0, a0, 16
  addi  t2, t2, 4
  blt   t2, t6, 1b
  vse32.v v8, (a3)
  ret
.size gemv4_int8, .-gemv4_int8

/* ===== 标准 RVV INT4 baseline（提取 4bit，符号扩展，vmul 累加）===== */
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

.globl gemv8_int4_base
.type  gemv8_int4_base, @function
gemv8_int4_base:
  li    t1, 0x600; csrs mstatus, t1
  vsetvli t0, x0, e16, m1, ta, ma
  vle16.v v8, (a2)
  li    t2, 0
  li    t3, 0xf
  vmv.v.x v1, t3
1:
  vle16.v v12, (a0)
  add   t4, a1, t2
  lh    t5, (t4)
  vmv.v.x v10, t5
  EXTRACT4 v20, v12, 0
  EXTRACT4 v21, v12, 4
  EXTRACT4 v22, v12, 8
  EXTRACT4 v23, v12, 12
  EXTRACT4 v24, v10, 0
  EXTRACT4 v25, v10, 4
  EXTRACT4 v26, v10, 8
  EXTRACT4 v27, v10, 12
  vmul.vv v28, v20, v24
  vmul.vv v29, v21, v25
  vmul.vv v30, v22, v26
  vmul.vv v31, v23, v27
  vadd.vv v9, v28, v29
  vadd.vv v9, v9, v30
  vadd.vv v9, v9, v31
  vadd.vv v8, v8, v9
  addi  a0, a0, 16
  addi  t2, t2, 2
  li    t6, 8
  blt   t2, t6, 1b
  vse16.v v8, (a3)
  ret
.size gemv8_int4_base, .-gemv8_int4_base

/* ===== 标准 RVV INT8 baseline（N 参数化）===== */
.globl gemv4_int8_base
.type  gemv4_int8_base, @function
gemv4_int8_base:
  li    t1, 0x600; csrs mstatus, t1
  vsetvli t0, x0, e32, m1, ta, ma
  vle32.v v8, (a2)
  mv    t2, x0
  mv    t6, a4
1:
  vle32.v v12, (a0)
  add   t4, a1, t2
  lw    t5, (t4)
  vmv.v.x v10, t5
  vsetvli t0, x0, e8, m1, ta, ma
  vwmul.vv v16, v12, v10
  vsetvli t0, x0, e16, m2, ta, ma
  vslide1down.vx v18, v16, x0
  vadd.vv v20, v16, v18
  li    t3, 2
  vslidedown.vx v22, v20, t3
  vadd.vv v24, v20, v22
  vid.v v28
  vsll.vi v28, v28, 2
  vrgather.vv v26, v24, v28
  li    t4, 4
  vsetvli t0, t4, e16, m1, ta, ma
  vwadd.vx v30, v26, x0
  vsetvli t0, t4, e32, m1, ta, ma
  vadd.vv v8, v8, v30
  vsetvli t0, x0, e32, m1, ta, ma
  addi  a0, a0, 16
  addi  t2, t2, 4
  blt   t2, t6, 1b
  vse32.v v8, (a3)
  ret
.size gemv4_int8_base, .-gemv4_int8_base
