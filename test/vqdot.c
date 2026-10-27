/*
 * CIE2026 AsyncSys — vqdot 端到端测试（Difftest + 程序自校验）
 *
 * 每条 vqdot 在 SEW=32 下把每个 32bit lane 重解释为 4 个 packed 8bit 子元素，
 * 做 4 项乘积归约并累加（wrapping on overflow）：
 *   vd[lane] = old_vd[lane] + Σ_{k=0..3} sext(vs1[lane][k]) * sext(vs2[lane][k])
 *
 * 本程序：
 *   1) 三组确定性边界用例（signed/unsigned 极值），直观打印；
 *   2) 每类型 100 组确定性伪随机回归（含 1/8 概率注入边界字节、非零累加）；
 *   3) Difftest 框架逐条对比 香山 RTL 与 NEMU 的 32 个向量寄存器。
 * 正常结束 HIT GOOD TRAP。
 */
#include <klib.h>
#include <stdint.h>

extern void vqdot_run(const void *vs1, const void *vs2,
                      const void *oldvd, void *out, int type);

/* type: 0 = vqdot (ss), 1 = vqdotu (uu), 2 = vqdotsu (vs2 signed x vs1 unsigned) */
static int32_t golden(const uint8_t *vs1, const uint8_t *vs2,
                      int32_t oldv, int type, int lane) {
  int32_t acc = 0;
  for (int k = 0; k < 4; k++) {
    int i = lane * 4 + k;
    int32_t x1 = (type == 0) ? (int32_t)(int8_t)vs1[i] : (int32_t)vs1[i];
    int32_t x2 = (type == 1) ? (int32_t)vs2[i] : (int32_t)(int8_t)vs2[i];
    acc += x1 * x2;
  }
  return acc + oldv;
}

static uint32_t out[4] __attribute__((aligned(16)));

/* 确定性 LCG，仿真结果可复现 */
static uint32_t rng = 0x78ac3d11u;
static uint32_t nxt(void) {
  rng = rng * 1103515245u + 12345u;
  return rng;
}

/* need_signed=1 时从有符号极值集合取，否则从无符号极值集合取 */
static uint8_t gen_byte(int need_signed) {
  uint32_t r = nxt();
  if ((r & 7u) == 0) {
    static const int8_t sb[8] = {0, 1, -1, 2, -2, 127, -128, -128};
    static const uint8_t ub[8] = {0, 1, 2, 255, 254, 128, 127, 64};
    return need_signed ? (uint8_t)sb[(r >> 3) & 7] : ub[(r >> 3) & 7];
  }
  return (uint8_t)(r >> 24);
}

static uint8_t b_vs1[16] __attribute__((aligned(16)));
static uint8_t b_vs2[16] __attribute__((aligned(16)));
static uint32_t b_old[4] __attribute__((aligned(16)));

/* 生成一组数据（随机/边界），执行并比对；print=1 时打印每个 lane */
static int check_group(const char *name, int type, int random_old, int print) {
  for (int lane = 0; lane < 4; lane++) {
    for (int k = 0; k < 4; k++) {
      int i = lane * 4 + k;
      b_vs1[i] = gen_byte(type == 0);   /* ss 时有符号，否则无符号 */
      b_vs2[i] = gen_byte(type != 1);   /* uu 时无符号，否则有符号 */
    }
    b_old[lane] = random_old ? (nxt() & 0xff) : 0;
  }
  vqdot_run(b_vs1, b_vs2, b_old, out, type);

  int ok = 1;
  for (int lane = 0; lane < 4; lane++) {
    int32_t exp = golden(b_vs1, b_vs2, (int32_t)b_old[lane], type, lane);
    int32_t got = (int32_t)out[lane];
    if (got != exp) {
      printf("[FAIL] %s lane %d: got %d exp %d\n", name, lane, got, exp);
      ok = 0;
    }
  }
  if (ok && print) {
    printf("[ OK ] %s: %d %d %d %d\n", name,
           (int)out[0], (int)out[1], (int)out[2], (int)out[3]);
  }
  return ok;
}

/* ---- 确定性边界用例（old=0） ---- */
static const uint8_t ss1[16] __attribute__((aligned(16))) = {
    1, 2, 3, 4,  -1, -2, -3, -4,  127, 127, 127, 127,  -128, -128, -128, -128};
static const uint8_t ss2[16] __attribute__((aligned(16))) = {
    1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1};
static const uint8_t uu1[16] __attribute__((aligned(16))) = {
    1, 2, 3, 4,  255, 255, 255, 255,  200, 200, 200, 200,  0, 0, 0, 0};
static const uint8_t uu2[16] __attribute__((aligned(16))) = {
    1, 1, 1, 1,  1, 1, 1, 1,  2, 2, 2, 2,  5, 5, 5, 5};
static const uint8_t su1[16] __attribute__((aligned(16))) = {
    1, 2, 3, 4,  255, 255, 255, 255,  255, 255, 255, 255,  10, 10, 10, 10};
static const uint8_t su2[16] __attribute__((aligned(16))) = {
    1, 1, 1, 1,  1, 1, 1, 1,  -1, -1, -1, -1,  -1, -1, -1, -1};
static const uint32_t z4[4] __attribute__((aligned(16))) = {0, 0, 0, 0};

static int boundary_case(const char *name, const uint8_t *v1,
                         const uint8_t *v2, int type) {
  vqdot_run(v1, v2, z4, out, type);
  int ok = 1;
  for (int lane = 0; lane < 4; lane++) {
    int32_t exp = golden(v1, v2, 0, type, lane);
    if ((int32_t)out[lane] != exp) {
      printf("[FAIL] %s lane %d: got %d exp %d\n", name, lane,
             (int32_t)out[lane], exp);
      ok = 0;
    }
  }
  if (ok) {
    printf("[ OK ] %s: %d %d %d %d\n", name,
           (int)out[0], (int)out[1], (int)out[2], (int)out[3]);
  }
  return ok;
}

int main() {
  int pass = 1;

  /* 对照①：标准 vmacc.vv（vs1=vs2=0，old 非零），结果应=old，验证 VIMac 读 oldVd 共性通路 */
  {
    static const uint32_t z32[4] __attribute__((aligned(16))) = {0};
    static const uint32_t ov[4] __attribute__((aligned(16))) = {100, 200, 300, 400};
    vqdot_run((const uint8_t *)z32, (const uint8_t *)z32, ov, out, 3);
    printf("VMACC-REF: %d %d %d %d (expect 100 200 300 400)\n",
           (int)out[0], (int)out[1], (int)out[2], (int)out[3]);
  }
  /* 诊断②：vqdot dot 非零(每lane=8)、old 非零，一次看清 src0/1(dot) 与 src2(old) */
  {
    static const uint8_t one16[16] __attribute__((aligned(16))) =
      {1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1};
    static const uint8_t two16[16] __attribute__((aligned(16))) =
      {2,2,2,2, 2,2,2,2, 2,2,2,2, 2,2,2,2};
    static const uint32_t ov[4] __attribute__((aligned(16))) = {100, 200, 300, 400};
    vqdot_run(one16, two16, ov, out, 0);
    printf("DOT+OLD ss: %d %d %d %d (expect 108 208 308 408)\n",
           (int)out[0], (int)out[1], (int)out[2], (int)out[3]);
  }
  /* 诊断③：vqdot dot=0、old 非零，单独验证 vdot 的 old vd 累加链路 */
  {
    static const uint8_t z16[16] __attribute__((aligned(16))) = {0};
    static const uint32_t ov[4] __attribute__((aligned(16))) = {100, 200, 300, 400};
    vqdot_run(z16, z16, ov, out, 0);
    printf("OLDONLY ss: %d %d %d %d (expect 100 200 300 400)\n",
           (int)out[0], (int)out[1], (int)out[2], (int)out[3]);
  }

  /* 边界用例 */
  pass &= boundary_case("vqdot   ss", ss1, ss2, 0);
  pass &= boundary_case("vqdotu  uu", uu1, uu2, 1);
  pass &= boundary_case("vqdotsu su", su1, su2, 2);

  /* 确定性伪随机回归：每类型 100 组，非零累加，不逐组打印 */
  const int N = 100;
  for (int t = 0; t < 3; t++) {
    const char *nm = (t == 0) ? "ss" : (t == 1) ? "uu" : "su";
    for (int it = 0; it < N; it++) {
      if (!check_group(nm, t, 1, 0)) {
        pass = 0;
        printf("  ^ at %s iteration %d\n", nm, it);
      }
    }
  }

  if (pass) {
    printf("VDOT All Tests PASS (3 boundary + 3x%d random groups)\n", N);
  } else {
    printf("VDOT Tests FAILED\n");
  }
  return 0;
}
