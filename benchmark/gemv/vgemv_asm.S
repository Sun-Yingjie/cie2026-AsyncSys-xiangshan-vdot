/*
 * CIE2026 AsyncSys — 量化 GEMV（线性层）端到端 kernel
 *
 * 计算 y = W·x + bias（INT8 权重/激活，INT32 累加），M=4 输出、N=16 输入。
 *
 * vqdot 用法：一条 vqdot（SEW=32，4 个 32bit 元素）同时处理
 *   4 个输出 × 4 个输入 的 tile；N 方向共 N/4=4 个 tile，逐 tile 累加。
 *
 * 权重布局 W_packed（离线重排，按 [tile][4 行]，每 tile 16 字节）：
 *   tile t 的 4 个 32bit 元素 k = W[k][4t..4t+3]
 * 输入 tile：x[4t..4t+3] 这 4 个字节经 vmv.v.x 广播到 4 个 32bit 元素。
 *
 * void gemv_vqdot(a0=Wpacked, a1=x, a2=bias, a3=y)
 */
.section .text
.option norvc

.macro VQDOT_VV vd, vs2, vs1
  .word (0xB2002057 + ((\vd) << 7) + ((\vs1) << 15) + ((\vs2) << 20))
.endm

.globl gemv_vqdot
.type gemv_vqdot, @function
gemv_vqdot:
  li    t1, 0x600
  csrs  mstatus, t1
  vsetvli t0, x0, e32, m1, ta, ma
  vle32.v v8, (a2)                 /* v8 = bias（4 个 int32） */
  li    t2, 0                      /* t2 = x/tile 字节偏移 */
1:
  vle32.v v12, (a0)                /* v12 = W tile：4 个 32bit（4 行各 4 权重） */
  add   t4, a1, t2
  lw    t5, (t4)                   /* t5 = x[4t..4t+3]（4 字节） */
  vmv.v.x v10, t5                  /* 广播到 4 个 32bit 元素 */
  VQDOT_VV 8, 12, 10               /* v8 += W·x（4 个输出的本 tile 部分和） */
  addi  a0, a0, 16                 /* 下一个 W tile */
  addi  t2, t2, 4
  li    t6, 16
  blt   t2, t6, 1b
  vse32.v v8, (a3)
  ret
.size gemv_vqdot, .-gemv_vqdot

/*
 * 标准 RVV GEMV：相同的 Wpacked/x/bias、相同的 tile 循环；
 * 唯一区别：tile 内"4 个 32bit 元素各自 4 个 int8 点积"用标准指令
 * （vwmul + 两级加法树 + 置换 + 加宽），再累加进 v8。
 *
 * void gemv_baseline(a0=Wpacked, a1=x, a2=bias, a3=y)
 */
.globl gemv_baseline
.type gemv_baseline, @function
gemv_baseline:
  li    t1, 0x600
  csrs  mstatus, t1
  vsetvli t0, x0, e32, m1, ta, ma
  vle32.v v8, (a2)                        /* v8 = bias */
  li    t2, 0
1:
  vle32.v v12, (a0)                       /* v12 = W tile */
  add   t4, a1, t2
  lw    t5, (t4)
  vmv.v.x v10, t5                         /* v10 = x tile 广播 */

  /* ---- tile 内：标准指令做 4 个 int32 点积 ---- */
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
  vwadd.vx v30, v26, x0                   /* 4 个 int16 -> 4 个 int32 */
  vsetvli t0, t4, e32, m1, ta, ma
  vadd.vv v8, v8, v30                     /* 累加本 tile */

  /* ---- 下一个 tile ---- */
  vsetvli t0, x0, e32, m1, ta, ma
  addi  a0, a0, 16
  addi  t2, t2, 4
  li    t6, 16
  blt   t2, t6, 1b
  vse32.v v8, (a3)
  ret
.size gemv_baseline, .-gemv_baseline
