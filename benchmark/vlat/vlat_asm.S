/*
 * CIE2026 AsyncSys — vqdot 单指令延迟 / 吞吐微基准
 *  (1) 无依赖 vqdot：16 目标轮转，稳态吞吐
 *  (2) 有依赖 vqdot：累加同一 v8，依赖延迟
 *  (3) 标准 RVV 基线序列（INT8 点积，严格对齐 vbench 成功版本），重复 N 次
 */
.section .text
.option norvc

.macro VQ VD, VS1, VS2
  .word (0xB2002057 + ((\VD) << 7) + ((\VS1) << 15) + ((\VS2) << 20))
.endm

/* ===== (1) 无依赖 vqdot：a0=N（16 的倍数）===== */
.globl vqdot_throughput_n
.type  vqdot_throughput_n, @function
vqdot_throughput_n:
  li    t1, 0x600; csrs mstatus, t1
  vsetvli t0, x0, e32, m1, ta, ma
  vmv.v.i v10, 3
  vmv.v.i v12, 2
  mv    t0, a0
  rdcycle t5
1:
  VQ 16, 10, 12
  VQ 20, 10, 12
  VQ 24, 10, 12
  VQ 28, 10, 12
  VQ 8,  10, 12
  VQ 4,  10, 12
  VQ 1,  10, 12
  VQ 5,  10, 12
  VQ 9,  10, 12
  VQ 13, 10, 12
  VQ 17, 10, 12
  VQ 21, 10, 12
  VQ 25, 10, 12
  VQ 29, 10, 12
  VQ 2,  10, 12
  VQ 6,  10, 12
  addi  t0, t0, -16
  bnez  t0, 1b
  rdcycle t6
  sub   a0, t6, t5
  ret
.size vqdot_throughput_n, .-vqdot_throughput_n

/* ===== (2) 有依赖 vqdot：a0=N ===== */
.globl vqdot_dep_n
.type  vqdot_dep_n, @function
vqdot_dep_n:
  li    t1, 0x600; csrs mstatus, t1
  vsetvli t0, x0, e32, m1, ta, ma
  vmv.v.i v10, 3
  vmv.v.i v12, 2
  vmv.v.i v8, 0
  mv    t0, a0
  rdcycle t5
1:
  VQ 8, 10, 12
  addi  t0, t0, -1
  bnez  t0, 1b
  rdcycle t6
  sub   a0, t6, t5
  ret
.size vqdot_dep_n, .-vqdot_dep_n

/* ===== (3) 标准 RVV INT8 基线序列，结果写 v8，old 用 v6 ===== */
.macro BASELINE_SEQ
  vsetvli t0, x0, e8, m1, ta, ma
  vwmul.vv v16, v14, v12          /* e8 -> e16，v16 m2 */
  vsetvli t0, x0, e16, m2, ta, ma
  vslide1down.vx v18, v16, x0
  vadd.vv v20, v16, v18
  li    t2, 2
  vslidedown.vx v22, v20, t2
  vadd.vv v24, v20, v22
  vid.v v28
  vsll.vi v28, v28, 2
  vrgather.vv v26, v24, v28       /* v26 = e16 点积 */
  li    t4, 4
  vsetvli t0, t4, e16, m1, ta, ma
  vwadd.vx v30, v26, x0           /* e16 -> e32，v30 m2 */
  vsetvli t0, t4, e32, m1, ta, ma
  vadd.vv v8, v30, v6             /* + old（e32） */
  vsetvli t0, x0, e8, m1, ta, ma
.endm

.globl baseline_seq_n
.type  baseline_seq_n, @function
baseline_seq_n:
  li    t1, 0x600; csrs mstatus, t1
  vsetvli t0, x0, e32, m1, ta, ma
  vmv.v.i v6, 0                   /* old = 0（e32） */
  vsetvli t0, x0, e8, m1, ta, ma
  vmv.v.i v12, 2
  vmv.v.i v14, 3
  mv    t3, a0                    /* 循环计数器用 t3（宏内 vsetvli 只用 t0/t2/t4） */
  rdcycle t5
1:
  BASELINE_SEQ
  addi  t3, t3, -1
  bnez  t3, 1b
  rdcycle t6
  sub   a0, t6, t5
  ret
.size baseline_seq_n, .-baseline_seq_n
