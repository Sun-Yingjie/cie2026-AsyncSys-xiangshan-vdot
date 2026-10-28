/*
 * CIE2026 AsyncSys — INT16 点积延迟对比（SEW=64）
 *
 * vqdot16：1 条自定义指令完成 2 个 64bit 累加器（每个累加 4 个 int16）。
 * base16：标准 RVV 实现——e16 加载 -> vwmul 加宽到 e32 -> 树形累加 -> vwadd 加宽到 e64。
 * 内存布局：8 个连续 int16（128bit），vqdot 按 e64 看、base 按 e16 看。
 */
.section .text
.option norvc

/* vqdot16：a0=A(vs2), a1=B(vs1), a2=old(vd), a3=out */
.globl vqdot16_k
.type  vqdot16_k, @function
vqdot16_k:
  li    t1, 0x600; csrs mstatus, t1
  vsetvli t0, x0, e64, m1, ta, ma
  vle64.v v8, (a2)
  vle64.v v12, (a0)
  vle64.v v10, (a1)
  .word  0xB2002057 | (8 << 7) | (10 << 15) | (12 << 20)
  vse64.v v8, (a3)
  ret
.size vqdot16_k, .-vqdot16_k

/* base16：标准 RVV 基线；a0=A, a1=B, a2=old, a3=out */
.globl base16
.type  base16, @function
base16:
  li    t1, 0x600; csrs mstatus, t1
  vsetvli t0, x0, e16, m1, ta, ma
  vle16.v v12, (a0)
  vle16.v v14, (a1)
  vwmul.vv v16, v14, v12          /* e16 -> e32，v16 为 m2（8 个 e32 乘积） */
  vsetvli t0, x0, e32, m2, ta, ma
  vslide1down.vx v18, v16, x0     /* prod[i+1] */
  vadd.vv v20, v16, v18           /* 两两和 */
  li    t2, 2
  vslidedown.vx v22, v20, t2
  vadd.vv v24, v20, v22           /* 位置0=sum(0..3)，位置4=sum(4..7) */
  vid.v v28
  vsll.vi v28, v28, 2             /* 索引 0,4,8,... */
  vrgather.vv v26, v24, v28       /* v26[0]=sum0, v26[1]=sum4 */
  vsetvli t0, t2, e32, m1, ta, ma
  vwadd.vx v30, v26, x0           /* e32 -> e64，2 个 e64 */
  vsetvli t0, t2, e64, m1, ta, ma
  vle64.v v28, (a2)
  vadd.vv v8, v30, v28
  vse64.v v8, (a3)
  ret
.size base16, .-base16
