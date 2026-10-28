/*
 * CIE2026 AsyncSys — 混合精度量化 MLP（INT4 层 + INT8 层）
 *
 *   x[16](int4) --W1(8x16,int4)+b1--> h1[8](int16)
 *               --ReLU+量化--> h8[8](int8)
 *               --W2(4x8,int8)+b2--> y[4](int32)
 */
#include <am.h>
#include <klib.h>
#include <stdint.h>

extern void gemv8_int4(const void *Wp, const void *xp, const void *b1, void *h1);
extern void gemv4_int8(const void *Wp, const void *h8, const void *b2, void *y, int N);
extern void gemv8_int4_base(const void *Wp, const void *xp, const void *b1, void *h1);
extern void gemv4_int8_base(const void *Wp, const void *h8, const void *b2, void *y, int N);

#define N1 16
#define M1 8
#define N2 8
#define M2 4

static inline uint64_t rdcycle(void) {
  uint64_t c; asm volatile("rdcycle %0" : "=r"(c)); return c;
}

static int8_t se4(int v) { v &= 0xf; return v >= 8 ? v - 16 : v; }

static uint16_t pack4(const int8_t *v) {
  uint16_t w = 0;
  for (int k = 0; k < 4; k++) w |= (uint16_t)(v[k] & 0xf) << (4 * k);
  return w;
}

static int8_t W1[M1][N1] __attribute__((aligned(16)));
static int8_t x[N1] __attribute__((aligned(16)));
static int16_t b1[M1] __attribute__((aligned(16)));
static uint16_t W1p[N1 / 4][M1] __attribute__((aligned(16)));
static uint16_t xp[N1 / 4] __attribute__((aligned(16)));
static int16_t h1[M1] __attribute__((aligned(16)));
static int8_t h8[N2] __attribute__((aligned(16)));

static int8_t W2[M2][N2] __attribute__((aligned(16)));
static int32_t b2[M2] __attribute__((aligned(16)));
static uint32_t W2p[N2 / 4][M2] __attribute__((aligned(16)));
static int32_t y[M2] __attribute__((aligned(16)));

/* baseline 工作区 */
static int16_t h1b[M1] __attribute__((aligned(16)));
static int8_t h8b[N2] __attribute__((aligned(16)));
static int32_t yb[M2] __attribute__((aligned(16)));

static void repack_all(void) {
  for (int t = 0; t < N1 / 4; t++) {
    for (int m = 0; m < M1; m++) W1p[t][m] = pack4(&W1[m][4 * t]);
    xp[t] = pack4(&x[4 * t]);
  }
  for (int t = 0; t < N2 / 4; t++)
    for (int k = 0; k < M2; k++) {
      uint32_t w = 0;
      for (int c = 0; c < 4; c++)
        w |= (uint8_t)W2[k][4 * t + c] << (8 * c);
      W2p[t][k] = w;
    }
}

static void golden(int32_t *out) {
  int16_t h[M1];
  for (int m = 0; m < M1; m++) {
    int acc = b1[m];
    for (int j = 0; j < N1; j++) acc += se4(W1[m][j]) * se4(x[j]);
    h[m] = (int16_t)acc;
  }
  int8_t hq[N2];
  for (int m = 0; m < M1; m++) {
    int v = h[m] > 0 ? h[m] : 0;
    hq[m] = v > 127 ? 127 : (int8_t)v;
  }
  for (int k = 0; k < M2; k++) {
    int32_t acc = b2[k];
    for (int m = 0; m < N2; m++) acc += (int32_t)W2[k][m] * hq[m];
    out[k] = acc;
  }
}

static void mixed_forward(void) {
  gemv8_int4(W1p, xp, b1, h1);
  for (int m = 0; m < M1; m++) {
    int v = h1[m] > 0 ? h1[m] : 0;
    h8[m] = v > 127 ? 127 : (int8_t)v;
  }
  gemv4_int8(W2p, h8, b2, y, N2);
}

static void mixed_baseline_forward(void) {
  gemv8_int4_base(W1p, xp, b1, h1b);
  for (int m = 0; m < M1; m++) {
    int v = h1b[m] > 0 ? h1b[m] : 0;
    h8b[m] = v > 127 ? 127 : (int8_t)v;
  }
  gemv4_int8_base(W2p, h8b, b2, yb, N2);
}

int main() {
  for (int j = 0; j < N1; j++) x[j] = (int8_t)((j % 4) - 1);
  for (int m = 0; m < M1; m++) {
    for (int j = 0; j < N1; j++) W1[m][j] = (int8_t)(((m + j) % 3) - 1);
    b1[m] = (int16_t)m;
  }
  for (int k = 0; k < M2; k++) {
    for (int m = 0; m < N2; m++) W2[k][m] = (int8_t)((m % 3) - 1);
    b2[k] = 10 * k;
  }
  repack_all();

  mixed_forward();
  mixed_baseline_forward();
  int32_t g[M2] __attribute__((aligned(16)));
  golden(g);
  int ok = 1;
  for (int k = 0; k < M2; k++) {
    if (y[k] != g[k] || yb[k] != g[k]) {
      printf("[FAIL] k=%d vq=%d base=%d exp=%d\n", k, y[k], yb[k], g[k]);
      ok = 0;
    }
  }
  printf("h1(int16):"); for (int m = 0; m < M1; m++) printf(" %d", h1[m]); printf("\n");
  printf("y:"); for (int k = 0; k < M2; k++) printf(" %d", y[k]); printf("\n");
  if (ok) printf("MIXED-PRECISION MLP PASS (both vqdot & baseline match golden)\n");

  const int R = 20;
  uint64_t t0 = rdcycle();
  for (int r = 0; r < R; r++) mixed_forward();
  uint64_t cm = (rdcycle() - t0) / R;
  t0 = rdcycle();
  for (int r = 0; r < R; r++) mixed_baseline_forward();
  uint64_t cb = (rdcycle() - t0) / R;
  printf("mixed vqdot   = %lu cyc\n", cm);
  printf("mixed baseline= %lu cyc\n", cb);
  printf("mixed speedup = %.2fx\n", (double)cb / cm);
  return 0;
}
