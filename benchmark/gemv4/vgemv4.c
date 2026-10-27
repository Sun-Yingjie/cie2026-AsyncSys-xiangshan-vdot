/*
 * CIE2026 AsyncSys — INT4 量化 GEMV（W4 线性层）端到端
 * y = W·x + bias，M=8 输出、N=16 输入；验证 + 延迟，HIT GOOD TRAP。
 */
#include <am.h>
#include <klib.h>
#include <stdint.h>

extern void gemv4_vqdot(const void *Wp, const void *x, const void *bias, void *y);
extern void gemv4_baseline(const void *Wp, const void *x, const void *bias, void *y);

#define M 8
#define N 16

static inline uint64_t rdcycle(void) {
  uint64_t c; asm volatile("rdcycle %0" : "=r"(c)); return c;
}

/* 4 个 int4 打包成 16bit */
static uint16_t pack4(const int8_t *v) {
  uint16_t w = 0;
  for (int k = 0; k < 4; k++) w |= ((uint16_t)(v[k] & 0xf)) << (4 * k);
  return w;
}

static int16_t golden(int m, const int8_t W[M][N], const int8_t *x, int16_t bias) {
  int acc = 0;
  for (int j = 0; j < N; j++) {
    int a = W[m][j] & 0xf, b = x[j] & 0xf;
    if (a & 8) a -= 16;
    if (b & 8) b -= 16;
    acc += a * b;
  }
  return (int16_t)(acc + bias);
}

int main() {
  static int8_t W[M][N], x[N];
  static int16_t bias[M];
  static uint16_t Wp[N / 4][M], xp[N / 4];
  static int16_t yv[M], yb[M];

  /* 确定性：x[j]=j-8，W[m][j]=m-4（每行恒定），bias=10m */
  for (int j = 0; j < N; j++) x[j] = (j % 16) - 8;
  for (int m = 0; m < M; m++) {
    for (int j = 0; j < N; j++) W[m][j] = m - 4;
    bias[m] = (int16_t)(10 * m);
  }
  /* 离线重排 W：按 [tile][8 行] */
  for (int t = 0; t < N / 4; t++)
    for (int m = 0; m < M; m++)
      Wp[t][m] = pack4(&W[m][4 * t]);
  for (int t = 0; t < N / 4; t++) xp[t] = pack4(&x[4 * t]);

  gemv4_vqdot(Wp, xp, bias, yv);
  gemv4_baseline(Wp, xp, bias, yb);

  int ok = 1;
  printf("vqdot:");
  for (int m = 0; m < M; m++) {
    int16_t e = golden(m, W, x, bias[m]);
    printf(" %d", yv[m]);
    if (yv[m] != e || yb[m] != e) { printf("\n[FAIL] m=%d vq=%d base=%d exp=%d", m, yv[m], yb[m], e); ok = 0; }
  }
  printf("\n");
  if (ok) printf("correctness PASS (y[m]=2m+32)\n");

  /* 随机 10 组 */
  uint32_t seed = 7;
  for (int r = 0; r < 10 && ok; r++) {
    for (int j = 0; j < N; j++) { seed = seed * 1103515245 + 12345; x[j] = (int8_t)((seed >> 16) % 16) - 8; }
    for (int m = 0; m < M; m++) {
      for (int j = 0; j < N; j++) { seed = seed * 1103515245 + 12345; W[m][j] = (int8_t)((seed >> 16) % 16) - 8; }
      seed = seed * 1103515245 + 12345; bias[m] = (int16_t)((seed >> 16) % 100 - 50);
    }
    for (int t = 0; t < N / 4; t++) {
      for (int m = 0; m < M; m++) Wp[t][m] = pack4(&W[m][4 * t]);
      xp[t] = pack4(&x[4 * t]);
    }
    gemv4_vqdot(Wp, xp, bias, yv);
    gemv4_baseline(Wp, xp, bias, yb);
    for (int m = 0; m < M; m++) {
      int16_t e = golden(m, W, x, bias[m]);
      if (yv[m] != e || yb[m] != e) { printf("[FAIL] rand r=%d m=%d\n", r, m); ok = 0; }
    }
  }
  if (ok) printf("random 10 groups PASS\n");

  /* 延迟：用确定性数据，R=20 */
  for (int j = 0; j < N; j++) x[j] = (j % 16) - 8;
  for (int m = 0; m < M; m++) { for (int j = 0; j < N; j++) W[m][j] = m - 4; bias[m] = (int16_t)(10 * m); }
  for (int t = 0; t < N / 4; t++) { for (int m = 0; m < M; m++) Wp[t][m] = pack4(&W[m][4 * t]); xp[t] = pack4(&x[4 * t]); }

  const int R = 20;
  uint64_t c0 = rdcycle();
  for (int r = 0; r < R; r++) gemv4_vqdot(Wp, xp, bias, yv);
  uint64_t c1 = rdcycle();
  for (int r = 0; r < R; r++) gemv4_baseline(Wp, xp, bias, yb);
  uint64_t c2 = rdcycle();
  uint64_t tv = (c1 - c0) / R, tb = (c2 - c1) / R;
  int ratio = (int)(tb * 100 / tv);
  printf("gemv4 vqdot: %d cycles\n", (int)tv);
  printf("gemv4 base : %d cycles\n", (int)tb);
  printf("speedup: %d.%02dx\n", ratio / 100, ratio % 100);
  return 0;
}
