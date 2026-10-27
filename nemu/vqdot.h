/***************************************************************************************
 * CIE2026 AsyncSys - vqdot.vv（对标 SiFive Xsfvqdotq）NEMU golden
 *
 * 指令在 SEW=32（vtype->vsew=2）下执行；每个 32bit 元素的 vs1/vs2 被重新解释为
 * 4 个 packed 8bit 子元素，子元素两两相乘并符号扩展到 32bit，4 项归约后累加到
 * 对应的 32bit vd（wrapping on overflow，不饱和）：
 *   vd[i] = old_vd[i] + Σ_{k=0..3} sign_extend(vs1[i][k]) * sign_extend(vs2[i][k])
 *
 *   vqdot   : vs2 signed   × vs1 signed
 *   vqdotu  : vs2 unsigned × vs1 unsigned
 *   vqdotsu : vs2 signed   × vs1 unsigned
 *
 * 元素以 EEW=32 经 get_vreg/set_vreg 访问，天然支持 LMUL；并按 RVV 规范处理
 * vm 掩码（vma）与 tail（vta）。SEW != 32 时触发非法指令（SiFive 规定 reserved）。
 ***************************************************************************************/

#include <common.h>
#ifdef CONFIG_RVV

#ifndef __RISCV64_VQDOT_H__
#define __RISCV64_VQDOT_H__

#include "vcompute_impl.h"

static inline void vqdot_core(Decode *s, int vs2_signed, int vs1_signed) {
  require_vector(true);
  vector_vvv_check(s, true);
  /* SiFive 语义：仅 SEW=32 合法 */
  if (vtype->vsew != 2) {
    longjmp_exception(EX_II);
  }
  check_vstart_exception(s);
  if (check_vstart_ignore(s)) {
    vp_set_dirty();
    return;
  }

  int vlmax = get_vlmax(vtype->vsew, vtype->vlmul);

  for (word_t idx = vstart->val; idx < vl->val; idx++) {
    /* 被 mask 掉的元素：不执行，按 vma 决定保持原值或置 agnostic */
    if (s->vm == 0) {
      rtlreg_t m = get_mask(0, idx);
      if (m == 0) {
        if (RVV_AGNOSTIC && vtype->vma) {
          set_vreg(id_dest->reg, idx, (uint64_t)-1, vtype->vsew, vtype->vlmul, 1);
        }
        continue;
      }
    }

    rtlreg_t a = 0, b = 0, oldv = 0;
    get_vreg(id_src->reg,  idx, &a,    vtype->vsew, vtype->vlmul, 0, 1); /* vs1 lane */
    get_vreg(id_src2->reg, idx, &b,    vtype->vsew, vtype->vlmul, 0, 1); /* vs2 lane */
    get_vreg(id_dest->reg, idx, &oldv, vtype->vsew, vtype->vlmul, 1, 1); /* vd old   */

    int32_t acc = 0;
    for (int k = 0; k < 4; k++) {
      uint8_t e1 = (uint8_t)((a >> (8 * k)) & 0xff);
      uint8_t e2 = (uint8_t)((b >> (8 * k)) & 0xff);
      int32_t x1 = vs1_signed ? (int32_t)(int8_t)e1 : (int32_t)e1;
      int32_t x2 = vs2_signed ? (int32_t)(int8_t)e2 : (int32_t)e2;
      acc += x1 * x2;
    }
    int32_t old = (int32_t)(oldv & 0xffffffffu);
    int32_t res = old + acc; /* 32bit 二补数自然回绕 */
    set_vreg(id_dest->reg, idx, (rtlreg_t)(uint32_t)res,
             vtype->vsew, vtype->vlmul, 1);
  }

  /* tail-agnostic：vl 之后的元素置为全 1 */
  if (RVV_AGNOSTIC && vtype->vta) {
    for (int idx = vl->val; idx < vlmax; idx++) {
      set_vreg(id_dest->reg, idx, (uint64_t)-1, vtype->vsew, vtype->vlmul, 1);
    }
  }

  rtl_li(s, s0, 0);
  vcsr_write(IDXVSTART, s0);
  vp_set_dirty();
}

def_EHelper(vqdot) {
  vqdot_core(s, 1, 1); /* signed × signed */
}

def_EHelper(vqdotu) {
  vqdot_core(s, 0, 0); /* unsigned × unsigned */
}

def_EHelper(vqdotsu) {
  vqdot_core(s, 1, 0); /* vs2 signed × vs1 unsigned */
}

#endif // __RISCV64_VQDOT_H__
#endif // CONFIG_RVV
