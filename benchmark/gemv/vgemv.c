/*
 * CIE2026 AsyncSys — 量化 GEMV（线性层）验证与计时
 *
 * 模型：y[k] = bias[k] + Σ_{j=0}^{15} W[k][j]·x[j]，INT8 输入、INT32 累加。
 * 先验证 vqdot kernel 与 golden 一致，再与标准 RVV kernel 对比端到端延迟。
 */
#include <klib.h>
#include <stdint.h>

#define M 4
#define N 16
#define TILE 4
#define NT (N / TILE)

extern void gemv_vqdot(const int8_t *Wpacked, const int8_t *x,
                       const int32_t *bias, int32_t *y);
extern void gemv_baseline(const int8_t *Wpacked, const int8_t *x,
                          const int32_t *bias, int32_t *y);

static int8_t W[M][N] __attribute__((aligned(16)));
static int8_t x[N] __attribute__((aligned(16)));
static int32_t bias[M] __attribute__((aligned(16)));
/* 重排后的权重：[tile][4 行 × 4 字节]，每 tile 16 字节 */
static int8_t Wpacked[NT][16] __attribute__((aligned(16)));
static int32_t y[M] __attribute__((aligned(16)));

static int32_t golden(int k) {
  int32_t acc = bias[k];
  for (int j = 0; j < N; j++) acc += (int32_t)W[k][j] * (int32_t)x[j];
  return acc;
}

static void repack(void) {
  for (int t = 0; t < NT; t++)
    for (int k = 0; k < M; k++)
      for (int c = 0; c < TILE; c++)
        Wpacked[t][k * TILE + c] = W[k][t * TILE + c];
}

static int32_t yb[M] __attribute__((aligned(16)));

static int check(const char *name,
                 void (*fn)(const int8_t *, const int8_t *,
                            const int32_t *, int32_t *),
                 int32_t *out) {
  fn((const int8_t *)Wpacked, x, bias, out);
  int ok = 1;
  for (int k = 0; k < M; k++) {
    int32_t exp = golden(k);
    if (out[k] != exp) {
      printf("[FAIL] %s y[%d]: got %d exp %d\n", name, k, out[k], exp);
      ok = 0;
    }
  }
  if (ok)
    printf("%s PASS: %d %d %d %d\n", name, out[0], out[1], out[2], out[3]);
  return ok;
}

static inline uint64_t rdcycle(void) {
  uint64_t c;
  asm volatile("rdcycle %0" : "=r"(c));
  return c;
}

static uint64_t timeit(
    void (*fn)(const int8_t *, const int8_t *, const int32_t *, int32_t *),
    int32_t *out) {
  const int R = 20;
  uint64_t t0 = rdcycle();
  for (int r = 0; r < R; r++) fn((const int8_t *)Wpacked, x, bias, out);
  return (rdcycle() - t0) / R;
}

int main() {
  for (int k = 0; k < M; k++) {
    for (int j = 0; j < N; j++) W[k][j] = (int8_t)(k + j - 10);
    bias[k] = 100 * k;
  }
  for (int j = 0; j < N; j++) x[j] = (int8_t)(j - 8);
  repack();

  int ok = 1;
  ok &= check("vqdot GEMV", gemv_vqdot, y);
  ok &= check("baseline GEMV", gemv_baseline, yb);

  if (ok) {
    uint64_t cq = timeit(gemv_vqdot, y);
    uint64_t cb = timeit(gemv_baseline, yb);
    printf("GEMV latency per kernel: vqdot %u cyc, baseline %u cyc, speedup %u.%02ux\n",
           (unsigned)cq, (unsigned)cb,
           (unsigned)(cb * 100 / cq) / 100,
           (unsigned)(cb * 100 / cq) % 100);
    printf("=== GEMV done ===\n");
  }
  return 0;
}
