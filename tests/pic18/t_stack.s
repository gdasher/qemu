; The return stack with STVREN off: 127 entries, STKOVF on the 128th push
; with the pointer held at 127, STKUNF on an empty pop, both cleared
; through PCON0, TOS reading zero when empty, nested calls, and the
; power-on state of PCON0/PCON1.
;
; SPDX-License-Identifier: GPL-2.0-or-later

    include "pic18test.inc"
    CONFIG MVECEN = OFF
    CONFIG STVREN = OFF

    PSECT resetvec,abs,class=CODE,reloc=2
    ORG 0
    goto    main

    PSECT text,abs,class=CODE,reloc=2
    ORG 0x100
main:
    movf    PCON0, w, a
    andlw   0xC0
    CHKW    0, 1
    movlw   127
    movwf   COUNT, a
p1:
    push
    decfsz  COUNT, f, a
    bra     p1
    movf    STKPTR, w, a
    CHKW    127, 2
    push
    movf    STKPTR, w, a
    CHKW    127, 3
    CHKSET  PCON0, 7, 4
    bcf     PCON0, 7, a
    CHKCLR  PCON0, 7, 5
    clrf    STKPTR, a
    pop
    CHKSET  PCON0, 6, 6
    movf    STKPTR, w, a
    CHKW    0, 7
    bcf     PCON0, 6, a
    CHKCLR  PCON0, 6, 8
; TOS reads zero with the stack empty
    movf    TOSL, w, a
    CHKW    0, 9
; nested calls
    call    d1
    CHKF    RESULT, 3, 10
    movf    STKPTR, w, a
    CHKW    0, 11
; PCON0 after power-on: nPOR and nBOR clear, nRI set; software sets them
    movf    PCON0, w, a
    andlw   0x07
    CHKW    0x04, 12
    bsf     PCON0, 1, a
    bsf     PCON0, 0, a
    movf    PCON0, w, a
    andlw   0x03
    CHKW    0x03, 13
; PCON1 reads nMEMV
    movf    PCON1, w, a
    CHKW    0x02, 14
    TESTEND

d1:
    call    d2
    return
d2:
    call    d3
    return
d3:
    movf    STKPTR, w, a
    movwf   RESULT, a
    return
    END
