/***************************************************************************************
 * CIE2026 AsyncSys - vqdot.vv（对标 SiFive Xsfvqdotq）NEMU golden
 *
 * 支持三种 SEW（SiFive 提案第 4 页 NOTE 定义的变体）：
 *   SEW=32（vsew=2）：每个 32bit 元素 packed 4 个 8bit 子元素 -> 累加到 32bit（标准）
 *   SEW=16（vsew=1）：每个 16bit 元素 packed 4 个 4bit 子元素 -> 累加到 16bit（INT4 变体）
 *   SEW=64（vsew=3）：每个 64bit 元素 packed 4 个 16bit 子元素 -> 累加到 64bit（INT16 变体）
 *
 * 通用语义（每组 4 个子元素，quad-widening）：
 *   vd[i] = old_vd[i] + Σ_{k=0..3} sign_extend(vs1[i][k]) * sign_extend(vs2[i][k])
 * （wrapping on overflow，不饱和）
 *
 *   vqdot   : vs2 signed   × vs1 signed
 *   vqdotu  : vs2 unsigned × vs1 unsigned
 *   vqdotsu : vs2 signed   × vs1 unsigned
 *
 * 元素以 EEW=SEW 经 get_vreg/set_vreg 访问，天然支持 LMUL；并按 RVV 规范处理
 * vm 掩码（vma）与 tail（vta）。SEW 不是 16/32/64 时触发非法指令。
 ***************************************************************************************/

#include <common.h>
#ifdef CONFIG_RVV

#ifndef __RISCV64_VQDOT_H__
#define __RISCV64_VQDOT_H__

#include "vcompute_impl.h"

/* 4bit 有符号扩展到 32bit */
static inline int32_t sext4(uint8_t e) {
  return (e & 0x8) ? (int32_t)(e) - 16 : (int32_t)e;
}

/* 取一个子元素（subbits 宽），按是否有符号扩展为 int64_t */
static inline int64_t sub_elem(uint64_t word, int k, int subbits,
                               int isInt4, int signed_) {
  uint64_t submask = (subbits == 64) ? ~0ULL : ((1ULL << subbits) - 1);
  uint64_t raw = (word >> (subbits * k)) & submask;
  if (!signed_) return (int64_t)raw;
  if (isInt4) return (int64_t)sext4((uint8_t)raw);
  if (subbits == 8)  return (int64_t)(int8_t)raw;
  if (subbits == 16) return (int64_t)(int16_t)raw;
  return (int64_t)raw; /* 不会发生 */
}

static inline void vqdot_core(Decode *s, int vs2_signed, int vs1_signed) {
  require_vector(true);
  vector_vvv_check(s, true);

  /* mode: 0=int8(SEW32), 1=int4(SEW16), 2=int16(SEW64)；其余非法 */
  int mode;
  if (vtype->vsew == 2)      mode = 0;
  else if (vtype->vsew == 1) mode = 1;
  else if (vtype->vsew == 3) mode = 2;
  else { longjmp_exception(EX_II); return; }

  const int isInt4 = (mode == 1);
  const int subbits = mode == 0 ? 8 : (mode == 1 ? 4 : 16);
  const uint64_t accmask = mode == 0 ? 0xffffffffULL
                         : (mode == 1 ? 0xffffULL : 0xffffffffffffffffULL);

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

    int64_t acc = 0;
    for (int k = 0; k < 4; k++) {
      int64_t x1 = sub_elem((uint64_t)a, k, subbits, isInt4, vs1_signed);
      int64_t x2 = sub_elem((uint64_t)b, k, subbits, isInt4, vs2_signed);
      acc += x1 * x2;
    }
    int64_t old = (int64_t)(oldv & accmask);
    int64_t res = old + acc; /* 累加器位宽二补数自然回绕 */
    set_vreg(id_dest->reg, idx, (rtlreg_t)((uint64_t)res & accmask),
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
