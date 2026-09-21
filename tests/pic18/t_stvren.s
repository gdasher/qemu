; With STVREN on (the default) a stack overflow resets the board and leaves
; STKOVF set, so the second pass through main sees it and passes.
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
    btfsc   PCON0, 7, a
    bra     after_overflow
    movlw   200
    movwf   COUNT, a
p1:
    push
    decfsz  COUNT, f, a
    bra     p1
    movlw   1
    bra     _fail
after_overflow:
    movf    STKPTR, w, a
    CHKW    0, 2
    bcf     PCON0, 7, a
    CHKCLR  PCON0, 7, 3
    TESTEND
    END
