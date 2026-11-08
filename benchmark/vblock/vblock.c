/*
 * CIE2026 AsyncSys — 深化端到端量化推理 demo（vblock）
 *
 * 在 vmlp（2 层 16->8->4）基础上，向真实 LLM 结构深化：
 *   3 层量化网络（32->16->16->8），并在同维层间加入残差连接（LLM 核心结构）。
 *
 *   x[32] --W1+b1--> z1[16] --ReLU+量化--> h1[16](int8)
 *         --W2+b2--> z2[16] + 残差h1 --ReLU+量化--> h2[16](int8)
 *         --W3+b3--> z3[8]  --ReLU--> y[8](int32)
 *
 * GEMV 复用参数化 gemv4 kernel（M=4，N 可变）；激活/量化/残差用标量（值少、可靠）。
 * 同时给出整数 C golden（含残差）逐层比对，并测端到端延迟。
 */
#include <klib.h>
#include <stdint.h>

#define N0 32
#define M1 16
#define M2 16
#define M3 8
#define TILE 4

extern void gemv4_vqdot(const int8_t *Wpacked, const int8_t *x,
                        const int32_t *bias, int32_t *y, int N);
extern void gemv4_baseline(const int8_t *Wpacked, const int8_t *x,
                           const int32_t *bias, int32_t *y, int N);

/* ---- 权重 / 输入 / 偏置 ---- */
static int8_t W1[M1][N0] __attribute__((aligned(16)));
static int8_t W2[M2][M1] __attribute__((aligned(16)));
static int8_t W3[M3][M2] __attribute__((aligned(16)));
static int8_t x[N0] __attribute__((aligned(16)));
static int32_t b1[M1], b2[M2], b3[M3];

/* ---- 重排后权重：[输出块][tile][16] ---- */
static int8_t W1p[(M1 / 4)][(N0 / 4)][16] __attribute__((aligned(16)));
static int8_t W2p[(M2 / 4)][(M1 / 4)][16] __attribute__((aligned(16)));
static int8_t W3p[(M3 / 4)][(M2 / 4)][16] __attribute__((aligned(16)));

/* 通用重排：扁平 W[M*N] -> Wp[M/4][N/4][16] */
static void repack(const int8_t *W, int8_t *Wp, int M, int N) {
  for (int b = 0; b < M / 4; b++)
    for (int t = 0; t < N / 4; t++)
      for (int k = 0; k < 4; k++)
        for (int c = 0; c < 4; c++)
          Wp[(b * (N / 4) + t) * 16 + k * 4 + c] = W[(4 * b + k) * N + (4 * t + c)];
}

static inline int8_t relu_q(int32_t v) {
  if (v < 0) v = 0;
  if (v > 127) v = 127;
  return (int8_t)v;
}

/* ================= vqdot 前向 ================= */
static int32_t z1[M1], z2[M2], z3[M3];
static int8_t h1[M1], h2[M2];

static void vqdot_block(void) {
  /* 层1：32->16，4 个输出块 */
  for (int b = 0; b < M1 / 4; b++)
    gemv4_vqdot((const int8_t *)&W1p[b][0], x, &b1[4 * b], &z1[4 * b], N0);
  for (int m = 0; m < M1; m++) h1[m] = relu_q(z1[m]);

  /* 层2：16->16 + 残差 h1 */
  for (int b = 0; b < M2 / 4; b++)
    gemv4_vqdot((const int8_t *)&W2p[b][0], h1, &b2[4 * b], &z2[4 * b], M1);
  for (int m = 0; m < M2; m++) h2[m] = relu_q(z2[m] + (int32_t)h1[m]);

  /* 层3：16->8，输出保留 int32 并 ReLU */
  for (int b = 0; b < M3 / 4; b++)
    gemv4_vqdot((const int8_t *)&W3p[b][0], h2, &b3[4 * b], &z3[4 * b], M2);
  for (int m = 0; m < M3; m++) if (z3[m] < 0) z3[m] = 0;
}

/* ================= baseline 前向 ================= */
static int32_t z1b[M1], z2b[M2], z3b[M3];
static int8_t h1b[M1], h2b[M2];

static void baseline_block(void) {
  for (int b = 0; b < M1 / 4; b++)
    gemv4_baseline((const int8_t *)&W1p[b][0], x, &b1[4 * b], &z1b[4 * b], N0);
  for (int m = 0; m < M1; m++) h1b[m] = relu_q(z1b[m]);

  for (int b = 0; b < M2 / 4; b++)
    gemv4_baseline((const int8_t *)&W2p[b][0], h1b, &b2[4 * b], &z2b[4 * b], M1);
  for (int m = 0; m < M2; m++) h2b[m] = relu_q(z2b[m] + (int32_t)h1b[m]);

  for (int b = 0; b < M3 / 4; b++)
    gemv4_baseline((const int8_t *)&W3p[b][0], h2b, &b3[4 * b], &z3b[4 * b], M2);
  for (int m = 0; m < M3; m++) if (z3b[m] < 0) z3b[m] = 0;
}

/* ================= C golden（含残差） ================= */
static void golden_block(int32_t *out) {
  int32_t g1[M1], g2[M2];
  int8_t q1[M1], q2[M2];
  for (int m = 0; m < M1; m++) {
    int32_t acc = b1[m];
    for (int j = 0; j < N0; j++) acc += (int32_t)W1[m][j] * (int32_t)x[j];
    g1[m] = acc;
    q1[m] = relu_q(g1[m]);
  }
  for (int m = 0; m < M2; m++) {
    int32_t acc = b2[m];
    for (int j = 0; j < M1; j++) acc += (int32_t)W2[m][j] * (int32_t)q1[j];
    g2[m] = acc + (int32_t)q1[m];          /* 残差 */
    q2[m] = relu_q(g2[m]);
  }
  for (int k = 0; k < M3; k++) {
    int32_t acc = b3[k];
    for (int j = 0; j < M2; j++) acc += (int32_t)W3[k][j] * (int32_t)q2[j];
    out[k] = acc > 0 ? acc : 0;
  }
}

static inline uint64_t rdcycle(void) {
  uint64_t c;
  asm volatile("rdcycle %0" : "=r"(c));
  return c;
}

int main() {
  for (int j = 0; j < N0; j++) x[j] = (int8_t)((j % 5) - 2);
  for (int m = 0; m < M1; m++) {
    for (int j = 0; j < N0; j++) W1[m][j] = (int8_t)(((m + j) % 4) - 1);
    b1[m] = m;
  }
  for (int m = 0; m < M2; m++) {
    for (int j = 0; j < M1; j++) W2[m][j] = (int8_t)(((m * 2 + j) % 3) - 1);
    b2[m] = 2 * m;
  }
  for (int m = 0; m < M3; m++) {
    for (int j = 0; j < M2; j++) W3[m][j] = (int8_t)((j % 4) - 1);
    b3[m] = 5 * m;
  }
  repack((const int8_t *)W1, (int8_t *)W1p, M1, N0);
  repack((const int8_t *)W2, (int8_t *)W2p, M2, M1);
  repack((const int8_t *)W3, (int8_t *)W3p, M3, M2);

  /* 正确性：vqdot vs golden */
  int32_t gy[M3] __attribute__((aligned(16)));
  golden_block(gy);
  vqdot_block();
  int ok = 1;
  for (int k = 0; k < M3; k++)
    if (z3[k] != gy[k]) { printf("[FAIL] vqdot y[%d]: got %d exp %d\n", k, z3[k], gy[k]); ok = 0; }
  printf("h1(int8):"); for (int m = 0; m < M1; m++) printf(" %d", h1[m]); printf("\n");
  printf("h2(int8):"); for (int m = 0; m < M2; m++) printf(" %d", h2[m]); printf("\n");
  if (ok) { printf("vblock vqdot PASS:"); for (int m = 0; m < M3; m++) printf(" %d", z3[m]); printf("\n"); }

  /* baseline 正确性 */
  baseline_block();
  int bok = 1;
  for (int k = 0; k < M3; k++)
    if (z3b[k] != gy[k]) { printf("[FAIL] baseline y[%d]: got %d exp %d\n", k, z3b[k], gy[k]); bok = 0; }
  if (bok) { printf("vblock baseline PASS:"); for (int m = 0; m < M3; m++) printf(" %d", z3b[m]); printf("\n"); }

  /* 端到端延迟（整个 3 层 block 前向） */
  const int R = 20;
  uint64_t t0 = rdcycle();
  for (int r = 0; r < R; r++) vqdot_block();
  uint64_t cq = (rdcycle() - t0) / R;
  t0 = rdcycle();
  for (int r = 0; r < R; r++) baseline_block();
  uint64_t cb = (rdcycle() - t0) / R;
  printf("block forward (3 layers %d->%d->%d->%d, residual): vqdot %d cyc vs baseline %d cyc, %.2fx\n",
         N0, M1, M2, M3, (int)cq, (int)cb, (double)cb / (double)cq);
  return 0;
}
