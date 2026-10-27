/*
 * CIE2026 AsyncSys — vdot latency 基准
 * 同一份 INT8 数据，对比：单条 vqdot vs 标准 RVV 组合（baseline）。
 */
#include <am.h>
#include <klib.h>
#include <stdint.h>

extern void dot_vqdot(void *A, void *B, void *old, void *out);
extern void dot_baseline(void *A, void *B, void *old, void *out);
extern uint64_t vqdot_compute_n(void *A, void *B, void *old, int n);
extern uint64_t base_compute_n(void *A, void *B, void *old, int n);

static int8_t   A[16] __attribute__((aligned(16)));
static int8_t   B[16] __attribute__((aligned(16)));
static int32_t oldv[4] __attribute__((aligned(16)));
static int32_t out_q[4] __attribute__((aligned(16)));
static int32_t out_b[4] __attribute__((aligned(16)));

static inline uint64_t rdcycle(void) {
  uint64_t c;
  asm volatile("rdcycle %0" : "=r"(c));
  return c;
}

/* C 语义参考：signed INT8, 4 个一组归约到 int32 */
static int32_t ref_dot(int j) {
  int32_t s = oldv[j];
  for (int k = 0; k < 4; k++)
    s += (int32_t)A[4*j+k] * (int32_t)B[4*j+k];
  return s;
}

/* 简单确定性伪随机（带符号 byte） */
static int8_t rnd8(int seed) {
  uint32_t x = (uint32_t)(seed * 1103515245u + 12345u);
  return (int8_t)((x >> 16) & 0xff);
}

static int check_correct(void) {
  int pass = 1;
  /* 一组覆盖正/负/边界的固定数据 */
  for (int i = 0; i < 16; i++) {
    A[i] = (int8_t)(i - 8);          /* -8..7 */
    B[i] = (int8_t)(7 - i);          /* 7..-8 */
  }
  for (int j = 0; j < 4; j++) oldv[j] = 100 * (j + 1);

  printf("step1: run dot_vqdot\n");
  dot_vqdot(A, B, oldv, out_q);
  printf("step2: q=[%d,%d,%d,%d]\n", out_q[0], out_q[1], out_q[2], out_q[3]);
  dot_baseline(A, B, oldv, out_b);
  printf("step3: b=[%d,%d,%d,%d]\n", out_b[0], out_b[1], out_b[2], out_b[3]);

  for (int j = 0; j < 4; j++) {
    int32_t r = ref_dot(j);
    if (out_q[j] != r || out_b[j] != r) {
      printf("MISMATCH j=%d ref=%d q=%d b=%d\n", j, r, out_q[j], out_b[j]);
      pass = 0;
    }
  }
  if (pass) {
    printf("correctness: vqdot==baseline==C-ref, out=[%d,%d,%d,%d]\n",
           out_q[0], out_q[1], out_q[2], out_q[3]);
  }
  return pass;
}

#define WARMUP 2
#define REPEAT 5

int main() {
  printf("=== vdot latency benchmark (INT8, 128-bit vector) ===\n");
  if (!check_correct()) {
    printf("BENCHMARK ABORTED (correctness failed)\n");
    return 0;
  }

  /* 随机填充一组用于计时 */
  for (int i = 0; i < 16; i++) { A[i] = rnd8(i+1); B[i] = rnd8(i+99); }
  for (int j = 0; j < 4; j++) oldv[j] = 0;

  for (int i = 0; i < WARMUP; i++) {
    dot_vqdot(A, B, oldv, out_q);
    dot_baseline(A, B, oldv, out_b);
  }

  uint64_t t0 = rdcycle();
  for (int i = 0; i < REPEAT; i++) dot_vqdot(A, B, oldv, out_q);
  uint64_t t1 = rdcycle();
  for (int i = 0; i < REPEAT; i++) dot_baseline(A, B, oldv, out_b);
  uint64_t t2 = rdcycle();

  uint64_t cq = (t1 - t0) / REPEAT;
  uint64_t cb = (t2 - t1) / REPEAT;
  printf("latency per kernel (whole, incl. load/store), R=%d:\n", REPEAT);
  printf("  vqdot    : %llu cycles\n", (unsigned long long)cq);
  printf("  baseline : %llu cycles\n", (unsigned long long)cb);
  printf("  speedup  : %.2fx\n", (double)cb / (double)cq);

  /* (B) 纯计算：load 一次，循环 N 次，反映指令本身的累加延迟 */
#define CN 10
  uint64_t pq = vqdot_compute_n(A, B, oldv, CN);
  uint64_t pb = base_compute_n(A, B, oldv, CN);
  uint64_t eq = pq / CN;
  uint64_t eb = pb / CN;
  printf("compute-only latency (accumulate, N=%d):\n", CN);
  printf("  vqdot    : %llu cycles\n", (unsigned long long)eq);
  printf("  baseline : %llu cycles\n", (unsigned long long)eb);
  printf("  speedup  : %.2fx\n", (double)eb / (double)eq);

  printf("=== benchmark done ===\n");
  return 0;
}
