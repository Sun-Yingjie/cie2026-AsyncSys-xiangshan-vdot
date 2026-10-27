/*
 * CIE2026 AsyncSys — INT4 vqdot 端到端测试（SEW=16）
 *
 * 32 个 int4（= 8 个 16bit 累加器），验证 ss/uu/su 三种点积；
 * Difftest 逐条对比 香山 RTL 与 NEMU。正常结束 HIT GOOD TRAP。
 */
#include <klib.h>
#include <stdint.h>

extern void dot4_ss(const void *A, const void *B, const void *old, void *out);
extern void dot4_uu(const void *A, const void *B, const void *old, void *out);
extern void dot4_su(const void *A, const void *B, const void *old, void *out);

#define NLANE 8   /* 8 个 16bit 累加器 */
#define NSUB  4   /* 每个累加器 4 个 int4 */

/* 把 4 个 4bit 子元素打包成 16bit */
static uint16_t pack4(const int8_t *v) {
  uint16_t w = 0;
  for (int k = 0; k < NSUB; k++) w |= ((uint16_t)(v[k] & 0xf)) << (4 * k);
  return w;
}

static int16_t golden(const int8_t *A, const int8_t *B,
                      int16_t oldv, int lane, int aSigned, int bSigned) {
  int acc = 0;
  for (int k = 0; k < NSUB; k++) {
    int i = lane * NSUB + k;
    int x = A[i] & 0xf, y = B[i] & 0xf;   // 先截断到 4bit
    if (aSigned && (x & 8)) x -= 16;
    if (bSigned && (y & 8)) y -= 16;
    acc += x * y;
  }
  return (int16_t)(acc + oldv);
}

typedef void (*fn4)(const void *, const void *, const void *, void *);

static int run_case(const char *name, fn4 fn,
                    int8_t A[32], int8_t B[32],
                    int aSigned, int bSigned, int16_t old[NLANE]) {
  static uint16_t Ap[NLANE], Bp[NLANE], out[NLANE];
  for (int l = 0; l < NLANE; l++) {
    Ap[l] = pack4(&A[l * NSUB]);
    Bp[l] = pack4(&B[l * NSUB]);
  }
  fn(Ap, Bp, old, out);
  int ok = 1;
  for (int l = 0; l < NLANE; l++) {
    int16_t exp = golden(A, B, old[l], l, aSigned, bSigned);
    if ((int16_t)out[l] != exp) {
      printf("[FAIL] %s lane %d: got %d exp %d\n", name, l, (int16_t)out[l], exp);
      ok = 0;
    }
  }
  if (ok) printf("%s PASS\n", name);
  return ok;
}

int main() {
  int ok = 1;
  int8_t A[32], B[32];
  int16_t old[NLANE];

  /* 确定性：A[i]=(i%16)-8（-8..7 循环），B[i]=7-(i%16)，old=10*lane */
  for (int i = 0; i < 32; i++) { A[i] = (i % 16) - 8; B[i] = 7 - (i % 16); }
  for (int l = 0; l < NLANE; l++) old[l] = (int16_t)(10 * l);
  ok &= run_case("int4 ss", dot4_ss, A, B, 1, 1, old);
  ok &= run_case("int4 su", dot4_su, A, B, 1, 0, old);

  /* 无符号：A[i]=i%16，B[i]=(15-i)%16，old=lane */
  for (int i = 0; i < 32; i++) { A[i] = i % 16; B[i] = (15 - i) % 16; }
  for (int l = 0; l < NLANE; l++) old[l] = (int16_t)l;
  ok &= run_case("int4 uu", dot4_uu, A, B, 0, 0, old);

  /* 随机回归 */
  uint32_t seed = 20261027;
  for (int t = 0; t < 60; t++) {
    for (int i = 0; i < 32; i++) {
      seed = seed * 1103515245 + 12345;
      A[i] = (int8_t)((seed >> 16) % 16) - 8;
      seed = seed * 1103515245 + 12345;
      B[i] = (int8_t)((seed >> 16) % 16) - 8;
    }
    for (int l = 0; l < NLANE; l++) {
      seed = seed * 1103515245 + 12345;
      old[l] = (int16_t)((seed >> 16) % 200 - 100);
    }
    if (!run_case("int4 ss rand", dot4_ss, A, B, 1, 1, old)) ok = 0;
    if (!run_case("int4 su rand", dot4_su, A, B, 1, 0, old)) ok = 0;
    for (int i = 0; i < 32; i++) {
      seed = seed * 1103515245 + 12345;
      A[i] = (int8_t)((seed >> 16) % 16);
      seed = seed * 1103515245 + 12345;
      B[i] = (int8_t)((seed >> 16) % 16);
    }
    if (!run_case("int4 uu rand", dot4_uu, A, B, 0, 0, old)) ok = 0;
  }

  if (ok) printf("=== INT4 vqdot all done ===\n");
  return 0;
}
