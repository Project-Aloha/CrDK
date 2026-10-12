#include "MpCrInternal.h"
.text
.global _start
_start:
  msr daifset, #15
  mrs x0, CurrentEL
  cmp x0, #12
  b.ne .Lhang
  mov x0, #0x42000000
  mov sp, x0
  adr x0, .Lvectors3
  msr vbar_el3, x0
  mov x0, #0x501
  msr scr_el3, x0
  msr cptr_el3, xzr
  mov x1, #0x80000000
#ifdef TEST_VHE
  orr x1, x1, #0x400000000
#endif
  msr hcr_el2, x1
  isb
  /* Copy the exact 4 KiB producer entry, then populate its immutable table. */
  adrp x0, MpCrTrampolineStart
  add x0, x0, :lo12:MpCrTrampolineStart
  mov x1, #0x41000000
  mov x2, #512
.Lcopy:
  ldr x3, [x0], #8
  str x3, [x1], #8
  subs x2, x2, #1
  b.ne .Lcopy
  mov x0, #0x41000000
  add x0, x0, #CR_MP_TABLE_OFFSET
  str xzr, [x0]
  mov x1, #0x41100000
  str x1, [x0, #8]
  /* Known identity-mapped Normal-NC lower 4 GiB, no boot image dependency. */
  mov x0, #0x41400000
  mov x1, #0x701
  str x1, [x0]
  mov x2, #0x40000000
  orr x1, x1, x2
  str x1, [x0, #8]
  mov x2, #0x80000000
  mov x1, #0x701
  orr x1, x1, x2
  str x1, [x0, #16]
  mov x2, #0xc0000000
  mov x1, #0x701
  orr x1, x1, x2
  str x1, [x0, #24]
  mov x0, #0x41100000
  movz w1, #(CR_MP_FRAME_MAGIC & 0xffff)
  movk w1, #(CR_MP_FRAME_MAGIC >> 16), lsl #16
  str w1, [x0, #CR_MP_FRAME_MAGIC_OFFSET]
  mov w1, #CR_MP_FRAME_REVISION
  str w1, [x0, #CR_MP_FRAME_REVISION_OFFSET]
#ifdef TEST_EL2
  mov w1, #2
#else
  mov w1, #1
#endif
  str w1, [x0, #CR_MP_FRAME_EL_OFFSET]
  mov w1, #1
  str w1, [x0, #CR_MP_FRAME_COMMAND_OFFSET]
  mov x1, #0x41200000
  str x1, [x0, #CR_MP_FRAME_STACK_OFFSET]
#ifdef TEST_EL2
  mov x1, #0x3520
  movk x1, #0x8080, lsl #16
#else
  mov x1, #0x3520
  movk x1, #0x80, lsl #16
  movk x1, #2, lsl #32
#endif
#ifdef TEST_BAD_PA
#ifdef TEST_EL2
  mov x2, #5
  bfi x1, x2, #16, #3
#else
  mov x2, #5
  bfi x1, x2, #32, #3
#endif
#endif
#ifdef TEST_BAD_TG
  orr x1, x1, #0x4000
#endif
#ifdef TEST_BAD_DS
#ifdef TEST_EL2
  orr x1, x1, #0x100000000
#else
  orr x1, x1, #0x800000000000000
#endif
#endif
#ifdef TEST_BAD_TG1
  bic x1, x1, #0x800000
  bic x1, x1, #0xc0000000
#endif
  str x1, [x0, #CR_MP_FRAME_TCR_OFFSET]
#ifdef TEST_WB
  mov x1, #0xff
#else
  mov x1, #0x44
#endif
  str x1, [x0, #CR_MP_FRAME_MAIR_OFFSET]
  mov x1, #0x41400000
  str x1, [x0, #CR_MP_FRAME_TTBR0_OFFSET]
#ifdef TEST_EL2
  mrs x1, sctlr_el2
#else
  mrs x1, sctlr_el1
#endif
  orr x1, x1, #1
  orr x1, x1, #4
  orr x1, x1, #0x1000
  bic x1, x1, #2
  str x1, [x0, #CR_MP_FRAME_SCTLR_OFFSET]
#ifdef TEST_EL2
  mov x1, #0x33ff
#else
  mov x1, #0x300000
#endif
  str x1, [x0, #CR_MP_FRAME_FP_OFFSET]
  mov x1, #0x41500000
  str x1, [x0, #CR_MP_FRAME_DISPATCH_OFFSET]
  adr x2, .Ldispatch
  mov x3, #16
.Lcopy_dispatch:
  ldr x4, [x2], #8
  str x4, [x1], #8
  subs x3, x3, #1
  b.ne .Lcopy_dispatch
  msr sctlr_el1, xzr
#ifdef TEST_EL2
  msr sctlr_el2, xzr
#endif
  /* Start with non-default floating point state; actual entry must reset it. */
  mov x1, #0x400000
  msr fpcr, x1
  mov x1, #0x9f
  msr fpsr, x1
  dsb sy
  isb
  mov x1, #0x41000000
  msr elr_el3, x1
#ifdef TEST_EL2
  mov x1, #0x3c9
#else
  mov x1, #0x3c5
#endif
  msr spsr_el3, x1
  eret
.Ldispatch:
  mov x1, #0x41300000
  mrs x2, CurrentEL
  str x2, [x1]
#ifdef TEST_EL2
  mrs x2, sctlr_el2
#else
  mrs x2, sctlr_el1
#endif
  str x2, [x1, #8]
  mrs x2, fpcr
  str x2, [x1, #16]
  mrs x2, fpsr
  str x2, [x1, #24]
  mrs x2, id_aa64mmfr2_el1
  str x2, [x1, #48]
#ifdef TEST_EL2
  mrs x2, cptr_el2
#else
  mrs x2, cpacr_el1
#endif
  str x2, [x1, #56]
  movz x2, #0xbeef
  movk x2, #0xdead, lsl #16
#ifdef TEST_EL2
  msr tpidr_el2, x2
#else
  msr tpidr_el1, x2
#endif
#ifdef TEST_FAULT
  brk #0
#endif
  ret
.Lmonitor:
  mov x16, #0x41300000
  mrs x17, esr_el3
  str x17, [x16, #32]
#ifdef TEST_EL2
  mrs x17, sctlr_el2
#else
  mrs x17, sctlr_el1
#endif
  str x17, [x16, #40]
  mov x0, #-3
  eret
.Lhang:
  wfe
  b .Lhang
.balign 2048
.Lvectors3:
.rept 16
  b .Lmonitor
  .space 124
.endr
