; Windows armasm64 spelling. WDK SDK macros generate unwind metadata.
; SPDX-License-Identifier: MIT
#include <kxarm64.h>
        NESTED_ENTRY CrPilArm64Smccc, |.text|
        PROLOG_SAVE_REG_PAIR x19, x20, #-32!
        PROLOG_SAVE_REG_PAIR_NO_FP fp, lr, #16
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
        EPILOG_RESTORE_REG_PAIR fp, lr, #16
        EPILOG_RESTORE_REG_PAIR x19, x20, #32!
        EPILOG_RETURN
        NESTED_END CrPilArm64Smccc
        LEAF_ENTRY CrPilArm64CurrentEl, |.text|
        mrs x0, CurrentEL
        lsr x0, x0, #2
        ret
        LEAF_END CrPilArm64CurrentEl
        END
