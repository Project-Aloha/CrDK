/** Private, position-independent PSCI AP entry. SPDX-License-Identifier: MIT */
#include "MpCrInternal.h"
.text
.balign 16
.global MpCrTrampolineStart
.global MpCrTrampolineEnd
MpCrTrampolineStart:
  msr daifset, #15
  mov x18, x0
  mov x19, xzr
  mrs x20, CurrentEL
  lsr x20, x20, #2
  cmp x20, #1
  b.eq .Larch_el1
  cmp x20, #2
  b.ne .Loff_raw
  /* The captured BSP register format is non-VHE EL2. Qualify the AP's
     own incoming format before touching its SCTLR or mailbox. */
  mrs x2, hcr_el2
  tbnz x2, #34, .Loff_unqualified
  mrs x2, sctlr_el2
  b .Larch_check
.Larch_el1:
  mrs x2, sctlr_el1
.Larch_check:
  tst x2, #1
  b.ne .Loff
  tst x2, #4
  b.ne .Loff
  /* Recover the frame from immutable reserved data, not an arbitrary context
     pointer or a software-defined TPIDR that a callback can legitimately use. */
  adr x3, .Lidentity
  mrs x1, mpidr_el1
  movz x2, #0xffff
  movk x2, #0xff, lsl #16
  movk x2, #0xff, lsl #32
  and x1, x1, x2
  mov x4, #CR_MP_MAX_CPUS
.Lentry_find:
  ldp x5, x6, [x3], #16
  cmp x5, x1
  b.eq .Lentry_found
  subs x4, x4, #1
  b.ne .Lentry_find
  b .Loff_unqualified
.Lentry_found:
  cbz x6, .Loff_unqualified
  cmp x6, x18
  b.ne .Loff_unqualified
  mov x19, x6
  ldr w1, [x19, #CR_MP_FRAME_MAGIC_OFFSET]
  movz w2, #(CR_MP_FRAME_MAGIC & 0xffff)
  movk w2, #(CR_MP_FRAME_MAGIC >> 16), lsl #16
  cmp w1, w2
  b.ne .Loff_unqualified
  ldr w1, [x19, #CR_MP_FRAME_REVISION_OFFSET]
  cmp w1, #CR_MP_FRAME_REVISION
  b.ne .Loff_unqualified
  ldr w1, [x19, #CR_MP_FRAME_EL_OFFSET]
  cmp w1, w20
  b.ne .Learly_fault
  mrs x1, mpidr_el1
  movz x2, #0xffff
  movk x2, #0xff, lsl #16
  movk x2, #0xff, lsl #32
  and x1, x1, x2
  ldr x2, [x19, #CR_MP_FRAME_AFFINITY_OFFSET]
  cmp x1, x2
  b.ne .Learly_fault
  /* Establish the dedicated stack before any C call or exception handler. */
  ldr x1, [x19, #CR_MP_FRAME_STACK_OFFSET]
  cbz x1, .Learly_fault
  tst x1, #15
  b.ne .Learly_fault
  msr spsel, #1
  mov sp, x1
  mov x29, xzr
  adr x1, .Lvectors
  cmp x20, #1
  b.ne .Lentry_el2
  msr vbar_el1, x1
  mrs x2, sctlr_el1
  b .Lentry_check
.Lentry_el2:
  msr vbar_el2, x1
  mrs x2, sctlr_el2
.Lentry_check:
  /* PSCI requires CPU_ON entry with translation/data cache disabled. */
  tst x2, #1
  b.ne .Learly_fault
  tst x2, #4
  b.ne .Learly_fault
  mrs x1, id_aa64mmfr2_el1
  ubfx x1, x1, #20, #4
  cmp x1, #1
  b.hi .Learly_fault
  isb
  mov w1, #CR_MP_STAGE_READY
  add x2, x19, #CR_MP_FRAME_STAGE_OFFSET
  stlr w1, [x2]
  dsb sy
.Lwait:
  add x2, x19, #CR_MP_FRAME_COMMAND_OFFSET
  ldar w1, [x2]
  cmp w1, #CR_MP_COMMAND_CLOSE
  b.eq .Loff
  cmp w1, #CR_MP_COMMAND_RUN
  b.eq .Lrun
  wfe
  b .Lwait
.Lrun:
  mov w1, #CR_MP_STAGE_RUNNING
  add x2, x19, #CR_MP_FRAME_STAGE_OFFSET
  stlr w1, [x2]
  ldr x1, [x19, #CR_MP_FRAME_TCR_OFFSET]
  ldr x2, [x19, #CR_MP_FRAME_MAIR_OFFSET]
  ldr x3, [x19, #CR_MP_FRAME_TTBR0_OFFSET]
  ldr x4, [x19, #CR_MP_FRAME_TTBR1_OFFSET]
  ldr x5, [x19, #CR_MP_FRAME_SCTLR_OFFSET]
  ldr x6, [x19, #CR_MP_FRAME_FP_OFFSET]
  /* Heterogeneous CPUs must implement the actual BSP translation format.
     The platform ArmMmuLib uses 4KiB TG0; no local PA width is guessed from
     topology. Its 52-bit format additionally requires local TGran4 LPA2. */
  mrs x7, id_aa64mmfr0_el1
  and x9, x7, #15
  cmp x9, #7
  b.hi .Learly_fault
  ubfx x8, x7, #28, #4
  cmp x8, #1
  b.hi .Learly_fault
  ubfx x7, x1, #14, #2
  cbnz x7, .Learly_fault
  cmp x20, #1
  b.ne .Lformat_el2
  ubfx x7, x1, #32, #3
  /* Check an enabled TTBR1's granule as well. */
  tbnz x1, #23, .Lformat_el1_ds
  ubfx x10, x1, #30, #2
  cmp x10, #2
  b.ne .Learly_fault
.Lformat_el1_ds:
  tbz x1, #59, .Lformat_without_ds
  b .Lformat_with_ds
.Lformat_el2:
  ubfx x7, x1, #16, #3
  tbz x1, #32, .Lformat_without_ds
.Lformat_with_ds:
  cmp x8, #1
  b.ne .Learly_fault
  b .Lformat_pa
.Lformat_without_ds:
  cmp x7, #6
  b.eq .Learly_fault
.Lformat_pa:
  cmp x7, #6
  b.hi .Learly_fault
  cmp x9, x7
  b.lo .Learly_fault
  cmp x20, #1
  b.ne .Lrun_el2
  tlbi vmalle1
  dsb sy
  msr tcr_el1, x1
  msr mair_el1, x2
  msr ttbr0_el1, x3
  msr ttbr1_el1, x4
  msr cpacr_el1, x6
  isb
  ic iallu
  dsb sy
  isb
  msr sctlr_el1, x5
  b .Lcall
.Lrun_el2:
  tlbi alle2
  dsb sy
  msr tcr_el2, x1
  msr mair_el2, x2
  msr ttbr0_el2, x3
  msr cptr_el2, x6
  isb
  ic iallu
  dsb sy
  isb
  msr sctlr_el2, x5
.Lcall:
  isb
  msr fpcr, xzr
  msr fpsr, xzr
  ldr x16, [x19, #CR_MP_FRAME_DISPATCH_OFFSET]
  cbz x16, .Learly_fault
  mov x0, x19
  blr x16
  /* Return to reserved code before signalling caller-argument retirement. */
  msr daifset, #15
  dsb sy
  mov w1, #CR_MP_STAGE_DONE
  add x2, x19, #CR_MP_FRAME_STAGE_OFFSET
  stlr w1, [x2]
  dsb sy
  b .Loff
.Learly_fault:
  mov x1, #-1
  str x1, [x19, #CR_MP_FRAME_FAULT_OFFSET]
  mov w1, #CR_MP_STAGE_FAULT
  add x2, x19, #CR_MP_FRAME_STAGE_OFFSET
  stlr w1, [x2]
  dsb sy
.Loff:
  /* Remain independent of the BSP's reclaimable translation tables even if
     firmware refuses CPU_OFF. Clean, do not invalidate, other CPUs' caches.
     The set/way geometry follows the architectural CCIDX formats. No stack
     or external function is accessed after the clean operation. */
  mrs x0, clidr_el1
  ubfx x1, x0, #24, #3
  lsl x1, x1, #1
  mov x2, #0
  mrs x10, csselr_el1
  mrs x11, id_aa64mmfr2_el1
  ubfx x11, x11, #20, #4
.Lcache_level:
  cmp x2, x1
  b.hs .Lcache_done
  add x3, x2, x2, lsr #1
  lsr x3, x0, x3
  and x3, x3, #7
  cmp x3, #2
  b.lo .Lcache_next
  msr csselr_el1, x2
  isb
  mrs x3, ccsidr_el1
  and x4, x3, #7
  add x4, x4, #4
  cbnz x11, .Lcache_ccidx
  ubfx x5, x3, #3, #10
  ubfx x7, x3, #13, #15
  b .Lcache_geometry
.Lcache_ccidx:
  ubfx x5, x3, #3, #21
  ubfx x7, x3, #32, #24
.Lcache_geometry:
  clz w6, w5
.Lcache_set:
  mov x8, x5
.Lcache_way:
  lsl x9, x8, x6
  orr x9, x9, x2
  lsl x12, x7, x4
  orr x9, x9, x12
  dc csw, x9
  subs x8, x8, #1
  b.ge .Lcache_way
  subs x7, x7, #1
  b.ge .Lcache_set
.Lcache_next:
  add x2, x2, #2
  b .Lcache_level
.Lcache_done:
  msr csselr_el1, x10
  dsb sy
  isb
  mrs x1, CurrentEL
  cmp x1, #4
  b.ne .Loff_el2
  mrs x1, sctlr_el1
  bic x1, x1, #1
  bic x1, x1, #4
  msr sctlr_el1, x1
  isb
  tlbi vmalle1
  b .Loff_fence
.Loff_el2:
  mrs x1, sctlr_el2
  bic x1, x1, #1
  bic x1, x1, #4
  msr sctlr_el2, x1
  isb
  tlbi alle2
.Loff_fence:
  dsb sy
  isb
  movz w0, #2
  movk w0, #0x8400, lsl #16
  mov x1, xzr
  mov x2, xzr
  mov x3, xzr
  mov x4, xzr
  mov x5, xzr
  mov x6, xzr
  mov x7, xzr
  mov x8, xzr
  mov x9, xzr
  mov x10, xzr
  mov x11, xzr
  mov x12, xzr
  mov x13, xzr
  mov x14, xzr
  mov x15, xzr
  mov x16, xzr
  mov x17, xzr
  smc #0
  cbz x19, .Lpark
  str x0, [x19, #CR_MP_FRAME_OFF_RESULT_OFFSET]
  mov w1, #CR_MP_STAGE_OFF_FAILED
  add x2, x19, #CR_MP_FRAME_STAGE_OFFSET
  stlr w1, [x2]
  dsb sy
.Lpark:
  wfe
  b .Lpark
.Loff_unqualified:
  mov x19, xzr
  b .Loff
.Loff_raw:
  movz w0, #2
  movk w0, #0x8400, lsl #16
  mov x1, xzr
  mov x2, xzr
  mov x3, xzr
  smc #0
  b .Lpark

.Lexception:
  msr daifset, #15
  mov x19, xzr
  adr x3, .Lidentity
  mrs x1, mpidr_el1
  movz x2, #0xffff
  movk x2, #0xff, lsl #16
  movk x2, #0xff, lsl #32
  and x1, x1, x2
  mov x4, #CR_MP_MAX_CPUS
.Lexception_find:
  ldp x5, x6, [x3], #16
  cmp x5, x1
  b.eq .Lexception_found
  subs x4, x4, #1
  b.ne .Lexception_find
  b .Loff_unqualified
.Lexception_found:
  mov x19, x6
  mrs x1, CurrentEL
  cmp x1, #4
  b.ne .Lexception_el2
  mrs x1, esr_el1
  b .Lexception_save
.Lexception_el2:
  mrs x1, esr_el2
.Lexception_save:
  cbz x19, .Loff_unqualified
  orr x1, x1, #0x8000000000000000
  str x1, [x19, #CR_MP_FRAME_FAULT_OFFSET]
  mov w1, #CR_MP_STAGE_FAULT
  add x2, x19, #CR_MP_FRAME_STAGE_OFFSET
  stlr w1, [x2]
  dsb sy
  b .Loff

.org CR_MP_TABLE_OFFSET
.Lidentity:
  .space CR_MP_MAX_CPUS * 16

/* One private vector table per copied entry page; no shared handler library. */
.balign 2048
.Lvectors:
.rept 16
  b .Lexception
  .space 124
.endr
/* Exception body precedes the table to keep the complete template one page. */
MpCrTrampolineEnd:

.global MpCrCaptureRegisters
MpCrCaptureRegisters:
  mrs x1, CurrentEL
  lsr x1, x1, #2
  str w1, [x0, #CR_MP_FRAME_EL_OFFSET]
  cmp x1, #1
  b.ne .Lcapture_el2
  mrs x1, tcr_el1
  mrs x2, mair_el1
  mrs x3, ttbr0_el1
  mrs x4, ttbr1_el1
  mrs x5, sctlr_el1
  mrs x6, cpacr_el1
  b .Lcapture_save
.Lcapture_el2:
  cmp x1, #2
  b.ne .Lcapture_return
  mrs x2, hcr_el2
  tbnz x2, #34, .Lcapture_invalid
  mrs x1, tcr_el2
  mrs x2, mair_el2
  mrs x3, ttbr0_el2
  mov x4, xzr
  mrs x5, sctlr_el2
  mrs x6, cptr_el2
.Lcapture_save:
  stp x1, x2, [x0, #CR_MP_FRAME_TCR_OFFSET]
  stp x3, x4, [x0, #CR_MP_FRAME_TTBR0_OFFSET]
  stp x5, x6, [x0, #CR_MP_FRAME_SCTLR_OFFSET]
.Lcapture_return:
  ret
.Lcapture_invalid:
  str wzr, [x0, #CR_MP_FRAME_EL_OFFSET]
  ret
.global MpCrWake
MpCrWake:
  dsb sy
  sev
  ret
