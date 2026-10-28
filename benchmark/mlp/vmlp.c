/*
 * CIE2026 AsyncSys — 端到端量化 MLP 推理 demo（INT8 vqdot）
 *
 * 2 层 MLP（对标 LLM 中 FFN 的堆叠结构）：
 *   x[16] --W1(8x16)+b1--> h1[8](int32) --ReLU + 量化--> h8[8](int8)
 *         --W2(4x8)+b2--> y[4](int32)
 *
 * GEMV 用 vqdot（参数化 kernel，M=4，N 可变）；中间 ReLU/量化值很少，用标量。
 * 同时给出整数 C golden 做逐元素比对，并测端到端延迟。
 */
#include <klib.h>
#include <stdint.h>

#define N1 16
#define M1 8
#define N2 8
#define M2 4
#define TILE 4
#define NT1 (N1 / TILE)
#define NT2 (N2 / TILE)

extern void gemv4_vqdot(const int8_t *Wpacked, const int8_t *x,
                        const int32_t *bias, int32_t *y, int N);
extern void gemv4_baseline(const int8_t *Wpacked, const int8_t *x,
                           const int32_t *bias, int32_t *y, int N);

static int8_t W1[M1][N1] __attribute__((aligned(16)));
static int8_t W2[M2][N2] __attribute__((aligned(16)));
static int8_t x[N1] __attribute__((aligned(16)));
static int32_t b1[M1] __attribute__((aligned(16)));
static int32_t b2[M2] __attribute__((aligned(16)));

/* 重排权重：[输出块][tile][16 字节] */
static int8_t W1packed[(M1 / 4)][NT1][16] __attribute__((aligned(16)));
static int8_t W2packed[NT2][16] __attribute__((aligned(16)));

static int32_t h1[M1] __attribute__((aligned(16)));
static int8_t h8[N2] __attribute__((aligned(16)));
static int32_t y[M2] __attribute__((aligned(16)));

static void repack(void) {
  for (int b = 0; b < M1 / 4; b++)
    for (int t = 0; t < NT1; t++)
      for (int k = 0; k < 4; k++)
        for (int c = 0; c < TILE; c++)
          W1packed[b][t][k * TILE + c] = W1[4 * b + k][4 * t + c];
  for (int t = 0; t < NT2; t++)
    for (int k = 0; k < 4; k++)
      for (int c = 0; c < TILE; c++)
        W2packed[t][k * TILE + c] = W2[k][4 * t + c];
}

/* 整数 C golden：完整 2 层 MLP */
static void golden_mlp(int32_t *out) {
  int32_t h[M1];
  for (int m = 0; m < M1; m++) {
    int32_t acc = b1[m];
    for (int j = 0; j < N1; j++) acc += (int32_t)W1[m][j] * (int32_t)x[j];
    h[m] = acc > 0 ? acc : 0;          /* ReLU */
  }
  for (int k = 0; k < M2; k++) {
    int32_t acc = b2[k];
    for (int m = 0; m < N2; m++) {
      int32_t q = h[m] > 127 ? 127 : h[m];   /* 饱和量化 */
      acc += (int32_t)W2[k][m] * q;
    }
    out[k] = acc;
  }
}

/* vqdot 2 层 MLP 前向 */
static void vqdot_mlp(void) {
  /* 第一层：M=8，两个输出块各 4 个输出 */
  gemv4_vqdot((const int8_t *)&W1packed[0], x, &b1[0], &h1[0], N1);
  gemv4_vqdot((const int8_t *)&W1packed[1], x, &b1[4], &h1[4], N1);
  /* ReLU + 饱和量化（中间值很少，标量处理） */
  for (int m = 0; m < M1; m++) {
    int32_t v = h1[m] > 0 ? h1[m] : 0;
    h8[m] = (int8_t)(v > 127 ? 127 : v);
  }
  /* 第二层：M=4，N=8 */
  gemv4_vqdot((const int8_t *)&W2packed[0], h8, &b2[0], &y[0], N2);
}

/* 标准 RVV 2 层 MLP 前向（相同数据，仅 GEMV 换成 baseline kernel） */
static int32_t h1b[M1] __attribute__((aligned(16)));
static int8_t h8b[N2] __attribute__((aligned(16)));
static int32_t yb[M2] __attribute__((aligned(16)));
static void baseline_mlp(void) {
  gemv4_baseline((const int8_t *)&W1packed[0], x, &b1[0], &h1b[0], N1);
  gemv4_baseline((const int8_t *)&W1packed[1], x, &b1[4], &h1b[4], N1);
  for (int m = 0; m < M1; m++) {
    int32_t v = h1b[m] > 0 ? h1b[m] : 0;
    h8b[m] = (int8_t)(v > 127 ? 127 : v);
  }
  gemv4_baseline((const int8_t *)&W2packed[0], h8b, &b2[0], &yb[0], N2);
}

static inline uint64_t rdcycle(void) {
  uint64_t c;
  asm volatile("rdcycle %0" : "=r"(c));
  return c;
}

int main() {
  for (int j = 0; j < N1; j++) x[j] = (int8_t)((j % 4) - 1);
  for (int m = 0; m < M1; m++) {
    for (int j = 0; j < N1; j++) W1[m][j] = (int8_t)(((m + j) % 3) - 1);
    b1[m] = m;
  }
  for (int k = 0; k < M2; k++) {
    for (int m = 0; m < N2; m++) W2[k][m] = (int8_t)((m % 3) - 1);
    b2[k] = 10 * k;
  }
  repack();

  /* 正确性：vqdot vs golden */
  vqdot_mlp();
  int32_t gy[M2] __attribute__((aligned(16)));
  golden_mlp(gy);
  int ok = 1;
  for (int k = 0; k < M2; k++) {
    if (y[k] != gy[k]) {
      printf("[FAIL] y[%d]: got %d exp %d\n", k, y[k], gy[k]);
      ok = 0;
    }
  }
  printf("hidden(int8 after ReLU): %d %d %d %d %d %d %d %d\n",
         h8[0], h8[1], h8[2], h8[3], h8[4], h8[5], h8[6], h8[7]);
  if (ok)
    printf("MLP vqdot PASS: %d %d %d %d\n", y[0], y[1], y[2], y[3]);

  /* baseline 正确性 */
  baseline_mlp();
  int bok = 1;
  for (int k = 0; k < M2; k++)
    if (yb[k] != gy[k]) {
      printf("[FAIL] baseline y[%d]: got %d exp %d\n", k, yb[k], gy[k]);
      bok = 0;
    }
  if (bok)
    printf("MLP baseline PASS: %d %d %d %d\n", yb[0], yb[1], yb[2], yb[3]);

  /* 端到端延迟对比（整个 2 层 MLP 前向） */
  const int R = 20;
  uint64_t t0 = rdcycle();
  for (int r = 0; r < R; r++) vqdot_mlp();
  uint64_t cq = (rdcycle() - t0) / R;
  t0 = rdcycle();
  for (int r = 0; r < R; r++) baseline_mlp();
  uint64_t cb = (rdcycle() - t0) / R;
  printf("MLP forward (2 layers, %d->%d->%d): vqdot %d cyc vs baseline %d cyc, %.2fx\n",
         N1, M1, M2, (int)cq, (int)cb, (double)cb / (double)cq);
  return 0;
}
