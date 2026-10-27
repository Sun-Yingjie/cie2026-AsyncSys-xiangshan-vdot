/*
 * CIE2026 AsyncSys — vdot 延迟基准
 * 两类对比：
 *  (A) 端到端 kernel：dot_vqdot / dot_baseline（含 load/store）
 *  (B) 纯计算 N 次  ：vqdot_compute_n / base_compute_n（load 一次，循环 N 次）
 *
 * 端到端：void dot_xxx(a0=A,a1=B,a2=old,a3=out)
 * 纯计算：uint64_t xxx_compute_n(a0=A,a1=B,a2=old,a3=N) -> N 次计算总周期
 *
 * RVV widening：vtype 设源（窄）SEW，目标自动 2SEW。
 */
.section .text
.option norvc

.macro VQDOT_VV vd, vs2, vs1
  .word (0xB2002057 + ((\vd) << 7) + ((\vs1) << 15) + ((\vs2) << 20))
.endm

# ============ (A) 端到端 ============
.globl dot_vqdot
.type dot_vqdot, @function
dot_vqdot:
  li    t1, 0x600
  csrs  mstatus, t1
  vsetvli t0, x0, e32, m1, ta, ma
  vle32.v v8, (a2)
  vle32.v v9, (a0)
  vle32.v v10, (a1)
  VQDOT_VV 8, 10, 9
  vse32.v v8, (a3)
  ret
.size dot_vqdot, .-dot_vqdot

.globl dot_baseline
.type dot_baseline, @function
dot_baseline:
  li    t1, 0x600
  csrs  mstatus, t1
  vsetvli t0, x0, e8, m1, ta, ma
  vle8.v v12, (a0)
  vle8.v v14, (a1)
  vwmul.vv v16, v14, v12
  vsetvli t0, x0, e16, m2, ta, ma
  vslide1down.vx v18, v16, x0
  vadd.vv v20, v16, v18
  li    t2, 2
  vslidedown.vx v22, v20, t2
  vadd.vv v24, v20, v22
  vid.v v28
  vsll.vi v28, v28, 2
  vrgather.vv v26, v24, v28
  li    t4, 4
  vsetvli t0, t4, e16, m1, ta, ma
  vwadd.vx v30, v26, x0
  vsetvli t0, t4, e32, m1, ta, ma
  vle32.v v28, (a2)
  vadd.vv v8, v30, v28
  vse32.v v8, (a3)
  ret
.size dot_baseline, .-dot_baseline

# ============ (B) 纯计算 N 次 ============
# vqdot：load 一次，循环 N 次 vqdot
.globl vqdot_compute_n
.type vqdot_compute_n, @function
vqdot_compute_n:
  li    t1, 0x600
  csrs  mstatus, t1
  vsetvli t0, x0, e32, m1, ta, ma
  vle32.v v8, (a2)
  vle32.v v9, (a0)
  vle32.v v10, (a1)
  rdcycle t5
  mv    t0, a3
1:
  VQDOT_VV 8, 10, 9
  addi  t0, t0, -1
  bnez  t0, 1b
  rdcycle t6
  sub   a0, t6, t5
  ret
.size vqdot_compute_n, .-vqdot_compute_n

# baseline：load 一次，循环 N 次"计算+累加"
.globl base_compute_n
.type base_compute_n, @function
base_compute_n:
  li    t1, 0x600
  csrs  mstatus, t1
  vsetvli t0, x0, e32, m1, ta, ma
  vle32.v v8, (a2)
  vsetvli t0, x0, e8, m1, ta, ma
  vle8.v v12, (a0)
  vle8.v v14, (a1)
  rdcycle t5
  mv    t0, a3
1:
  # vwmul (e8->e16)
  vwmul.vv v16, v14, v12
  vsetvli t4, x0, e16, m2, ta, ma
  vslide1down.vx v18, v16, x0
  vadd.vv v20, v16, v18
  li    t2, 2
  vslidedown.vx v22, v20, t2
  vadd.vv v24, v20, v22
  vid.v v28
  vsll.vi v28, v28, 2
  vrgather.vv v26, v24, v28
  # widening 到 e32，累加 v8
  li    t4, 4
  vsetvli t4, t4, e16, m1, ta, ma
  vwadd.vx v30, v26, x0
  vsetvli t4, t4, e32, m1, ta, ma
  vadd.vv v8, v8, v30
  # 回到 e8 准备下一次（源 v12/v14 内容未变，只需切 vtype）
  vsetvli t4, x0, e8, m1, ta, ma
  addi  t0, t0, -1
  bnez  t0, 1b
  rdcycle t6
  sub   a0, t6, t5
  ret
.size base_compute_n, .-base_compute_n
