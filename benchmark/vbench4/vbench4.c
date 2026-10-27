/*
 * CIE2026 AsyncSys — INT4 点积延迟对比（SEW=16）
 * 验证 vqdot4 / base4 正确性，rdcycle 测延迟，HIT GOOD TRAP。
 */
#include <am.h>
#include <klib.h>
#include <stdint.h>

static inline uint64_t rdcycle(void) {
  uint64_t c;
  asm volatile("rdcycle %0" : "=r"(c));
  return c;
}

extern void vqdot4(const void *A, const void *B, const void *old, void *out);
extern void base4(const void *A, const void *B, const void *old, void *out);

#define NLANE 8
#define NSUB  4

static uint16_t pack4(const int8_t *v) {
  uint16_t w = 0;
  for (int k = 0; k < NSUB; k++) w |= ((uint16_t)(v[k] & 0xf)) << (4 * k);
  return w;
}

static int16_t golden(const int8_t *A, const int8_t *B, int16_t oldv, int lane) {
  int acc = 0;
  for (int k = 0; k < NSUB; k++) {
    int i = lane * NSUB + k;
    int x = A[i] & 0xf, y = B[i] & 0xf;
    if (x & 8) x -= 16;
    if (y & 8) y -= 16;
    acc += x * y;
  }
  return (int16_t)(acc + oldv);
}

int main() {
  static int8_t A[32], B[32];
  static uint16_t Ap[NLANE], Bp[NLANE], old[NLANE], ov[NLANE], ob[NLANE];
  int ok = 1;

  /* 确定性 + 随机 30 组正确性 */
  uint32_t seed = 42;
  for (int t = 0; t < 30; t++) {
    for (int i = 0; i < 32; i++) {
      seed = seed * 1103515245 + 12345; A[i] = (int8_t)((seed >> 16) % 16) - 8;
      seed = seed * 1103515245 + 12345; B[i] = (int8_t)((seed >> 16) % 16) - 8;
    }
    for (int l = 0; l < NLANE; l++) {
      seed = seed * 1103515245 + 12345;
      old[l] = (uint16_t)(int16_t)((seed >> 16) % 100 - 50);
      Ap[l] = pack4(&A[l * NSUB]); Bp[l] = pack4(&B[l * NSUB]);
    }
    vqdot4(Ap, Bp, old, ov);
    base4(Ap, Bp, old, ob);
    for (int l = 0; l < NLANE; l++) {
      int16_t e = golden(A, B, (int16_t)old[l], l);
      if ((int16_t)ov[l] != e || (int16_t)ob[l] != e) {
        printf("[FAIL] t=%d l=%d vq=%d base=%d exp=%d\n", t, l,
               (int16_t)ov[l], (int16_t)ob[l], e);
        ok = 0;
      }
    }
  }
  if (ok) printf("correctness PASS (30 groups)\n");

  /* 延迟：固定数据，R 次 */
  for (int i = 0; i < 32; i++) { A[i] = (i % 16) - 8; B[i] = 7 - (i % 16); }
  for (int l = 0; l < NLANE; l++) { old[l] = 0; Ap[l] = pack4(&A[l * NSUB]); Bp[l] = pack4(&B[l * NSUB]); }

  const int R = 20;
  uint64_t c0 = rdcycle();
  for (int r = 0; r < R; r++) vqdot4(Ap, Bp, old, ov);
  uint64_t c1 = rdcycle();
  for (int r = 0; r < R; r++) base4(Ap, Bp, old, ob);
  uint64_t c2 = rdcycle();

  uint64_t tv = (c1 - c0) / R, tb = (c2 - c1) / R;
  printf("vqdot4: %d cycles\n", (int)tv);
  printf("base4 : %d cycles\n", (int)tb);
  printf("speedup: 1.%02dx\n", (int)(tb * 100 / tv) - 100);
  return 0;
}
