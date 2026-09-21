; The RESET instruction resets the board and leaves nRI clear in PCON0, so
; the second pass through main sees it and passes.
;
; SPDX-License-Identifier: GPL-2.0-or-later

    include "pic18test.inc"
    CONFIG MVECEN = OFF

    PSECT resetvec,abs,class=CODE,reloc=2
    ORG 0
    goto    main

    PSECT text,abs,class=CODE,reloc=2
    ORG 0x100
main:
    btfss   PCON0, 2, a
    bra     after_reset
; first pass: leave something on the stack, then reset
    push
    reset
    movlw   1
    bra     _fail
after_reset:
; the stack pointer is back at zero
    movf    STKPTR, w, a
    CHKW    0, 2
; and the power-on causes are still recorded
    movf    PCON0, w, a
    andlw   0x07
    CHKW    0, 3
    TESTEND
    END
