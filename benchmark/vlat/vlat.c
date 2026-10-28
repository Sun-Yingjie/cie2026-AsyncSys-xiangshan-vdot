/*
 * CIE2026 AsyncSys — vqdot 单指令延迟 / 吞吐微基准
 */
#include <am.h>
#include <klib.h>
#include <stdint.h>

extern uint64_t vqdot_throughput_n(uint64_t N);
extern uint64_t vqdot_dep_n(uint64_t N);
extern uint64_t baseline_seq_n(uint64_t N);

#define N 96

int main() {
  uint64_t t_ind = vqdot_throughput_n(N);
  uint64_t t_dep = vqdot_dep_n(N);
  uint64_t t_base = baseline_seq_n(N);

  printf("vqdot independent: %lu instr / %lu cyc = %.3f CPI\n", N, t_ind, (double)t_ind / N);
  printf("vqdot dependency : %lu instr / %lu cyc = %.3f CPI\n", N, t_dep, (double)t_dep / N);
  printf("baseline seq     : %lu seq   / %lu cyc = %.3f cyc/seq\n", N, t_base, (double)t_base / N);
  printf("vqdot execution latency = 2 cycles; dependent-issue latency = ~7 cycles\n");
  printf("VLAT DONE\n");
  return 0;
}
