// Windows ARM64 GAS/LLVM spelling; instruction-identical to armasm64 source.
// SPDX-License-Identifier: MIT
.text
.p2align 2
.globl CrPilArm64Smccc
.seh_proc CrPilArm64Smccc
CrPilArm64Smccc:
  stp x19, x20, [sp, #-32]!
  .seh_save_regp_x x19, 32
  stp x29, x30, [sp, #16]
  .seh_save_fplr 16
  .seh_endprologue
  mov x19, x0
  ldp x0, x1, [x19, #0]
  ldp x2, x3, [x19, #16]
  ldp x4, x5, [x19, #32]
  ldp x6, x7, [x19, #48]
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
  stp x0, x1, [x19, #0]
  stp x2, x3, [x19, #16]
  stp x4, x5, [x19, #32]
  stp x6, x7, [x19, #48]
  .seh_startepilogue
  ldp x29, x30, [sp, #16]
  .seh_save_fplr 16
  ldp x19, x20, [sp], #32
  .seh_save_regp_x x19, 32
  .seh_endepilogue
  ret
.seh_endproc

.p2align 2
.globl CrPilArm64CurrentEl
.seh_proc CrPilArm64CurrentEl
CrPilArm64CurrentEl:
  .seh_endprologue
  mrs x0, CurrentEL
  lsr x0, x0, #2
  ret
.seh_endproc
