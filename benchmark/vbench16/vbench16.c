/*
 * CIE2026 AsyncSys — INT16 点积延迟对比（SEW=64）
 * 验证 vqdot16_k / base16 正确性，rdcycle 测延迟，HIT GOOD TRAP。
 */
#include <am.h>
#include <klib.h>
#include <stdint.h>

static inline uint64_t rdcycle(void) {
  uint64_t c;
  asm volatile("rdcycle %0" : "=r"(c));
  return c;
}

extern void vqdot16_k(const void *A, const void *B, const void *old, void *out);
extern void base16(const void *A, const void *B, const void *old, void *out);

#define NLANE 2
#define NSUB  4
#define NELEM (NLANE * NSUB)   /* 8 个 int16 */

static int64_t golden(const int16_t *A, const int16_t *B, int64_t oldv, int lane) {
  int64_t acc = 0;
  for (int k = 0; k < NSUB; k++) {
    int i = lane * NSUB + k;
    acc += (int64_t)A[i] * B[i];
  }
  return acc + oldv;
}

int main() {
  static int16_t A[NELEM], B[NELEM];
  static int64_t oldv[NLANE], g[NLANE];
  static int64_t rq[NLANE], rb[NLANE];

  /* 确定性数据 */
  for (int i = 0; i < NELEM; i++) {
    A[i] = (i % 5) - 2;
    B[i] = (i % 4) - 1;
  }
  for (int l = 0; l < NLANE; l++) {
    oldv[l] = 100 * (l + 1);
    g[l] = golden(A, B, oldv[l], l);
  }

  uint64_t c0 = rdcycle();
  vqdot16_k(A, B, oldv, rq);
  uint64_t c1 = rdcycle();
  base16(A, B, oldv, rb);
  uint64_t c2 = rdcycle();

  int fail = 0;
  printf("vqdot16: ");
  for (int l = 0; l < NLANE; l++) {
    printf("%ld ", rq[l]);
    if (rq[l] != g[l]) fail = 1;
  }
  printf("\nbase16:  ");
  for (int l = 0; l < NLANE; l++) {
    printf("%ld ", rb[l]);
    if (rb[l] != g[l]) fail = 1;
  }
  printf("\ngolden:  ");
  for (int l = 0; l < NLANE; l++) printf("%ld ", g[l]);
  printf("\n");

  uint64_t tq = c1 - c0, tb = c2 - c1;
  printf("vqdot16 cycles = %lu\n", tq);
  printf("base16  cycles = %lu\n", tb);
  printf("speedup = %.2fx\n", (double)tb / tq);

  if (fail) {
    printf("RESULT FAIL\n");
    while (1);
  }
  printf("INT16 BENCH PASS\n");
  return 0;
}
