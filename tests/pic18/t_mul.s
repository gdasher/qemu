; Hardware multiply: MULLW and MULWF into PRODH:PRODL.
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
    movlw   0x10
    mullw   0x10
    movf    PRODL, w, a
    CHKW    0x00, 1
    movf    PRODH, w, a
    CHKW    0x01, 2
    movlw   0xFF
    mullw   0xFF
    movf    PRODL, w, a
    CHKW    0x01, 3
    movf    PRODH, w, a
    CHKW    0xFE, 4
    movlw   0
    mullw   0x55
    movf    PRODL, w, a
    CHKW    0, 5
    movf    PRODH, w, a
    CHKW    0, 6
    movlw   0x12
    movwf   SCRATCH, a
    movlw   0x34
    mulwf   SCRATCH, a
    movf    PRODL, w, a
    CHKW    0xA8, 7
    movf    PRODH, w, a
    CHKW    0x03, 8
    movlb   5
    movlw   200
    movwf   0x90, b
    mulwf   0x90, b
    movf    PRODL, w, a
    CHKW    0x40, 9
    movf    PRODH, w, a
    CHKW    0x9C, 10
    movlw   0x0F
    mulwf   WREG, a
    movf    PRODL, w, a
    CHKW    0xE1, 11
    movf    PRODH, w, a
    CHKW    0, 12
; PRODL and PRODH are ordinary writable registers
    movlw   0xAB
    movwf   PRODH, a
    movf    PRODH, w, a
    CHKW    0xAB, 13
    TESTEND
    END
