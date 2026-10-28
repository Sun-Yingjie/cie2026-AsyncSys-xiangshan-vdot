/*
 * CIE2026 AsyncSys — 端到端量化 MLP 推理 kernel（INT8 vqdot）
 *
 * 参数化 GEMV：一个向量固定 4 个输出（M=4），输入维度 N 可变（N 为 4 的倍数）。
 * 权重布局 Wpacked：[tile][16 字节]，tile t 的 4 个 32bit 元素 k = W[k][4t..4t+3]。
 *
 * void gemv4_vqdot(a0=Wpacked, a1=x, a2=bias, a3=y, a4=N)
 */
.section .text
.option norvc

.macro VQDOT_VV vd, vs2, vs1
  .word (0xB2002057 + ((\vd) << 7) + ((\vs1) << 15) + ((\vs2) << 20))
.endm

.globl gemv4_vqdot
.type gemv4_vqdot, @function
gemv4_vqdot:
  li    t1, 0x600
  csrs  mstatus, t1
  vsetvli t0, x0, e32, m1, ta, ma
  vle32.v v8, (a2)                 /* v8 = bias（4 个 int32） */
  mv    t2, x0                     /* t2 = x/tile 字节偏移 */
  mv    t6, a4                     /* t6 = N（循环上界） */
1:
  vle32.v v12, (a0)                /* v12 = W tile（4 行各 4 权重） */
  add   t4, a1, t2
  lw    t5, (t4)                   /* t5 = x[4t..4t+3] */
  vmv.v.x v10, t5                  /* 广播到 4 个 32bit 元素 */
  VQDOT_VV 8, 12, 10               /* v8 += W·x（4 输出本 tile 部分和） */
  addi  a0, a0, 16                 /* 下一个 W tile */
  addi  t2, t2, 4
  blt   t2, t6, 1b
  vse32.v v8, (a3)
  ret
.size gemv4_vqdot, .-gemv4_vqdot

/*
 * 标准 RVV baseline：相同 Wpacked/x/bias、相同 tile 循环，N 可变（a4）；
 * tile 内点积用标准指令（vwmul + 两级加法树 + 置换 + 加宽）。
 *
 * void gemv4_baseline(a0=Wpacked, a1=x, a2=bias, a3=y, a4=N)
 */
.globl gemv4_baseline
.type gemv4_baseline, @function
gemv4_baseline:
  li    t1, 0x600
  csrs  mstatus, t1
  vsetvli t0, x0, e32, m1, ta, ma
  vle32.v v8, (a2)                        /* v8 = bias */
  mv    t2, x0
  mv    t6, a4                            /* t6 = N */
1:
  vle32.v v12, (a0)
  add   t4, a1, t2
  lw    t5, (t4)
  vmv.v.x v10, t5

  vsetvli t0, x0, e8, m1, ta, ma
  vwmul.vv v16, v12, v10                  /* 16 个 int8 乘积 -> e16 m2 */
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
  vwadd.vx v30, v26, x0                   /* 4 个 int16 -> int32 */
  vsetvli t0, t4, e32, m1, ta, ma
  vadd.vv v8, v8, v30                     /* 累加本 tile */

  vsetvli t0, x0, e32, m1, ta, ma
  addi  a0, a0, 16
  addi  t2, t2, 4
  blt   t2, t6, 1b
  vse32.v v8, (a3)
  ret
.size gemv4_baseline, .-gemv4_baseline
