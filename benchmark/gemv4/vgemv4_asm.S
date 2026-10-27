/*
 * CIE2026 AsyncSys — INT4 量化 GEMV（W4 线性层）端到端 kernel
 *
 * y = W·x + bias（INT4 权重/激活，INT16 累加），M=8 输出、N=16 输入。
 *
 * 一条 vqdot（SEW=16，8 个 16bit 元素）同时处理 8 输出 × 4 输入 的 tile；
 * N 方向共 N/4=4 个 tile，逐 tile 累加。
 *
 * 权重布局 W_packed（离线重排，按 [tile][8 行]，每 tile 16 字节）：
 *   tile t 的 8 个 16bit 元素 m = W[m][4t..4t+3] 打包
 * 输入 tile：x[4t..4t+3] 4 个 int4 打包成 16bit，vmv.v.x 广播到 8 个元素。
 *
 * void gemv4_vqdot(a0=Wpacked, a1=x, a2=bias, a3=y)
 */
.section .text
.option norvc

.macro VQDOT_VV vd, vs2, vs1
  .word (0xB2002057 + ((\vd) << 7) + ((\vs1) << 15) + ((\vs2) << 20))
.endm

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

.globl gemv4_vqdot
.type  gemv4_vqdot, @function
gemv4_vqdot:
  li    t1, 0x600; csrs mstatus, t1
  vsetvli t0, x0, e16, m1, ta, ma
  vle16.v v8, (a2)                 /* v8 = bias（8 个 int16） */
  li    t2, 0
1:
  vle16.v v12, (a0)                /* v12 = W tile：8 个 16bit */
  add   t4, a1, t2
  lh    t5, (t4)                   /* t5 = x[4t..4t+3] 打包 */
  vmv.v.x v10, t5                  /* 广播到 8 个 16bit */
  VQDOT_VV 8, 12, 10
  addi  a0, a0, 16
  addi  t2, t2, 2
  li    t6, 8
  blt   t2, t6, 1b
  vse16.v v8, (a3)
  ret
.size gemv4_vqdot, .-gemv4_vqdot

/*
 * 标准 RVV INT4 GEMV：相同 Wpacked/x/bias、相同 tile 循环；
 * tile 内用 EXTRACT4 提取 4bit、符号扩展、vmul、向量累加。
 *
 * void gemv4_baseline(a0=Wpacked, a1=x, a2=bias, a3=y)
 */
.globl gemv4_baseline
.type  gemv4_baseline, @function
gemv4_baseline:
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

  /* W 子元素 v20..v23 */
  EXTRACT4 v20, v12, 0
  EXTRACT4 v21, v12, 4
  EXTRACT4 v22, v12, 8
  EXTRACT4 v23, v12, 12
  /* x 子元素 v24..v27 */
  EXTRACT4 v24, v10, 0
  EXTRACT4 v25, v10, 4
  EXTRACT4 v26, v10, 8
  EXTRACT4 v27, v10, 12
  /* 乘积 v28..v31 */
  vmul.vv v28, v20, v24
  vmul.vv v29, v21, v25
  vmul.vv v30, v22, v26
  vmul.vv v31, v23, v27
  /* 累加进 v8 */
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
.size gemv4_baseline, .-gemv4_baseline
