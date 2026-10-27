/*
 * CIE2026 AsyncSys — INT16 vqdot 端到端测试（SEW=64）
 *
 * 8 个 int16（= 2 个 64bit 累加器），验证 ss/uu/su 三种点积；
 * Difftest 逐条对比 香山 RTL 与 NEMU。正常结束 HIT GOOD TRAP。
 */
#include <klib.h>
#include <stdint.h>

extern void dot16_ss(const void *A, const void *B, const void *old, void *out);
extern void dot16_uu(const void *A, const void *B, const void *old, void *out);
extern void dot16_su(const void *A, const void *B, const void *old, void *out);

#define NLANE 2   /* 2 个 64bit 累加器 */
#define NSUB  4   /* 每个累加器 4 个 int16 */

/* 把 4 个 int16 子元素打包成 64bit */
static uint64_t pack16(const int16_t *v) {
  uint64_t w = 0;
  for (int k = 0; k < NSUB; k++) w |= ((uint64_t)(uint16_t)v[k]) << (16 * k);
  return w;
}

static int64_t golden(const int16_t *A, const int16_t *B,
                      int64_t oldv, int lane, int aSigned, int bSigned) {
  int64_t acc = 0;
  for (int k = 0; k < NSUB; k++) {
    int i = lane * NSUB + k;
    int64_t x = aSigned ? (int64_t)A[i] : (int64_t)(uint16_t)A[i];
    int64_t y = bSigned ? (int64_t)B[i] : (int64_t)(uint16_t)B[i];
    acc += x * y;
  }
  return acc + oldv;
}

typedef void (*fn16)(const void *, const void *, const void *, void *);

static int run_case(const char *name, fn16 fn,
                    int16_t A[8], int16_t B[8],
                    int aSigned, int bSigned, int64_t old[NLANE]) {
  static uint64_t Ap[NLANE], Bp[NLANE], out[NLANE];
  for (int l = 0; l < NLANE; l++) {
    Ap[l] = pack16(&A[l * NSUB]);
    Bp[l] = pack16(&B[l * NSUB]);
  }
  fn(Ap, Bp, old, out);
  int ok = 1;
  for (int l = 0; l < NLANE; l++) {
    int64_t exp = golden(A, B, old[l], l, aSigned, bSigned);
    if ((int64_t)out[l] != exp) {
      printf("[FAIL] %s lane %d: got %lld exp %lld\n", name, l,
             (long long)(int64_t)out[l], (long long)exp);
      ok = 0;
    }
  }
  if (ok) printf("%s PASS\n", name);
  return ok;
}

int main() {
  int ok = 1;
  int16_t A[8], B[8];
  int64_t old[NLANE];

  /* 确定性：A[i]=i-4（-4..3），B[i]=7-i，old=1000*lane */
  for (int i = 0; i < 8; i++) { A[i] = i - 4; B[i] = 7 - i; }
  for (int l = 0; l < NLANE; l++) old[l] = (int64_t)(1000 * l);
  ok &= run_case("int16 ss", dot16_ss, A, B, 1, 1, old);
  ok &= run_case("int16 su", dot16_su, A, B, 1, 0, old);

  /* 无符号：A[i]=i，B[i]=15-i，old=100*lane */
  for (int i = 0; i < 8; i++) { A[i] = i; B[i] = 15 - i; }
  for (int l = 0; l < NLANE; l++) old[l] = (int64_t)(100 * l);
  ok &= run_case("int16 uu", dot16_uu, A, B, 0, 0, old);

  /* 随机回归 */
  uint32_t seed = 20261027;
  for (int t = 0; t < 60; t++) {
    for (int i = 0; i < 8; i++) {
      seed = seed * 1103515245 + 12345;
      A[i] = (int16_t)((seed >> 16) % 2000 - 1000);
      seed = seed * 1103515245 + 12345;
      B[i] = (int16_t)((seed >> 16) % 2000 - 1000);
    }
    for (int l = 0; l < NLANE; l++) {
      seed = seed * 1103515245 + 12345;
      old[l] = (int64_t)((seed >> 16) % 20000 - 10000);
    }
    if (!run_case("int16 ss rand", dot16_ss, A, B, 1, 1, old)) ok = 0;
    if (!run_case("int16 su rand", dot16_su, A, B, 1, 0, old)) ok = 0;
    for (int i = 0; i < 8; i++) {
      seed = seed * 1103515245 + 12345;
      A[i] = (int16_t)((seed >> 16) % 2000);
      seed = seed * 1103515245 + 12345;
      B[i] = (int16_t)((seed >> 16) % 2000);
    }
    if (!run_case("int16 uu rand", dot16_uu, A, B, 0, 0, old)) ok = 0;
  }

  if (ok) printf("=== INT16 vqdot all done ===\n");
  return 0;
}
