; Arithmetic and logic: every flag of every ALU instruction, against the
; values worked out by hand from DS40002147F chapter 44.
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
; ADDLW 0x7F + 0x01 = 0x80: N OV DC
    movlw   0x7F
    addlw   0x01
    CHKWS   0x80, 0x1A, 1
; 0xFF + 0x01 = 0x00: Z DC C
    movlw   0xFF
    addlw   0x01
    CHKWS   0x00, 0x07, 3
; 0x80 + 0x80 = 0x00: OV Z C
    movlw   0x80
    addlw   0x80
    CHKWS   0x00, 0x0D, 5
; SUBLW 5 - 3 = 2: no borrow, so C and DC set
    movlw   3
    sublw   5
    CHKWS   0x02, 0x03, 7
; SUBLW 3 - 5 = 0xFE: borrow, N
    movlw   5
    sublw   3
    CHKWS   0xFE, 0x10, 9
; ADDWF 0x0F + 0x01 = 0x10: DC only
    movlw   0x0F
    movwf   SCRATCH, a
    movlw   0x01
    addwf   SCRATCH, f, a
    CHKST   0x02, 11
    CHKF    SCRATCH, 0x10, 12
; ADDWFC with C set: 0x10 + 0x01 + 1 = 0x12
    bsf     STATUS, C_BIT, a
    movlw   0x01
    addwfc  SCRATCH, w, a
    CHKWS   0x12, 0x00, 13
; SUBWF 0x10 - 0x10 = 0: Z DC C
    movlw   0x10
    subwf   SCRATCH, w, a
    CHKWS   0x00, 0x07, 15
; SUBWFB with C clear: 0x10 - 0x01 - 1 = 0x0E, no borrow out
    bcf     STATUS, C_BIT, a
    movlw   0x01
    subwfb  SCRATCH, w, a
    CHKWS   0x0E, 0x01, 17
; SUBFWB with C set: W 0x20 - f 0x10 - 0 = 0x10: C DC
    bsf     STATUS, C_BIT, a
    movlw   0x20
    subfwb  SCRATCH, w, a
    CHKWS   0x10, 0x03, 19
; NEGF 0x01 = 0xFF: N only
    movlw   1
    movwf   SCRATCH, a
    negf    SCRATCH, a
    CHKST   0x10, 21
    CHKF    SCRATCH, 0xFF, 22
; NEGF 0x80 = 0x80: N OV DC
    movlw   0x80
    movwf   SCRATCH, a
    negf    SCRATCH, a
    CHKST   0x1A, 23
    CHKF    SCRATCH, 0x80, 24
; NEGF 0 = 0: Z DC C
    clrf    SCRATCH, a
    negf    SCRATCH, a
    CHKST   0x07, 25
; DECF 0 = 0xFF: borrow, N
    clrf    SCRATCH, a
    decf    SCRATCH, f, a
    CHKST   0x10, 26
    CHKF    SCRATCH, 0xFF, 27
; DECF 0x80 = 0x7F: OV C
    movlw   0x80
    movwf   SCRATCH, a
    decf    SCRATCH, f, a
    CHKST   0x09, 28
    CHKF    SCRATCH, 0x7F, 29
; DECF 1 = 0: Z DC C
    movlw   1
    movwf   SCRATCH, a
    decf    SCRATCH, w, a
    CHKWS   0x00, 0x07, 30
; INCF 0x7F = 0x80: N OV DC
    movlw   0x7F
    movwf   SCRATCH, a
    incf    SCRATCH, f, a
    CHKST   0x1A, 32
    CHKF    SCRATCH, 0x80, 33
; INCF 0xFF = 0: Z DC C
    setf    SCRATCH, a
    incf    SCRATCH, f, a
    CHKST   0x07, 34
    CHKF    SCRATCH, 0x00, 35
; ANDLW leaves C, DC, OV alone: 0xF0 & 0x0F = 0 with Z
    movlw   0xF0
    andlw   0x0F
    CHKWS   0x00, 0x07, 36
; IORLW: N
    movlw   0x00
    iorlw   0x80
    CHKWS   0x80, 0x13, 38
; XORLW back to zero: Z
    movlw   0x80
    xorlw   0x80
    CHKWS   0x00, 0x07, 40
; ANDWF, IORWF, XORWF with f
    movlw   0xAA
    movwf   SCRATCH, a
    movlw   0x0F
    andwf   SCRATCH, w, a
    CHKW    0x0A, 42
    movlw   0x55
    iorwf   SCRATCH, w, a
    CHKW    0xFF, 43
    movlw   0xAA
    xorwf   SCRATCH, w, a
    CHKWS   0x00, 0x07, 44
; COMF 0xAA = 0x55, then 0x7F = 0x80 with N
    comf    SCRATCH, w, a
    CHKW    0x55, 46
    CHKCLR  STATUS, N_BIT, 47
    movlw   0x7F
    movwf   SCRATCH, a
    comf    SCRATCH, w, a
    CHKSET  STATUS, N_BIT, 48
; RLCF with C set: 0x80 -> 0x01, C set
    bsf     STATUS, C_BIT, a
    movlw   0x80
    movwf   SCRATCH, a
    rlcf    SCRATCH, f, a
    CHKF    SCRATCH, 0x01, 49
    CHKSET  STATUS, C_BIT, 50
; RRCF with C clear: 0x01 -> 0x00, C and Z set
    bcf     STATUS, C_BIT, a
    rrcf    SCRATCH, f, a
    CHKSET  STATUS, C_BIT, 51
    CHKSET  STATUS, Z_BIT, 52
    CHKF    SCRATCH, 0x00, 53
; RRCF with C set: 0x00 -> 0x80, C clear, N set
    bsf     STATUS, C_BIT, a
    rrcf    SCRATCH, f, a
    CHKCLR  STATUS, C_BIT, 54
    CHKSET  STATUS, N_BIT, 55
    CHKF    SCRATCH, 0x80, 56
; RLNCF 0x81 -> 0x03, RRNCF back
    movlw   0x81
    movwf   SCRATCH, a
    rlncf   SCRATCH, f, a
    CHKCLR  STATUS, N_BIT, 57
    CHKF    SCRATCH, 0x03, 58
    rrncf   SCRATCH, f, a
    CHKSET  STATUS, N_BIT, 59
    CHKF    SCRATCH, 0x81, 60
; SWAPF 0x81 -> 0x18, flags untouched
    bsf     STATUS, N_BIT, a
    swapf   SCRATCH, w, a
    movwf   RESULT, a
    CHKSET  STATUS, N_BIT, 61
    CHKF    RESULT, 0x18, 62
; SETF and CLRF
    setf    SCRATCH, a
    CHKF    SCRATCH, 0xFF, 62
    bcf     STATUS, Z_BIT, a
    clrf    SCRATCH, a
    CHKSET  STATUS, Z_BIT, 63
    CHKF    SCRATCH, 0x00, 64
; MOVF sets Z and N
    movf    SCRATCH, w, a
    CHKSET  STATUS, Z_BIT, 65
    movlw   0x80
    movwf   SCRATCH, a
    movf    SCRATCH, f, a
    CHKSET  STATUS, N_BIT, 66
    CHKCLR  STATUS, Z_BIT, 67
; DAW: the data sheet's two examples, and one that needs no adjustment
    bcf     STATUS, C_BIT, a
    bcf     STATUS, DC_BIT, a
    movlw   0xA5
    daw
    CHKW    0x05, 68
    CHKSET  STATUS, C_BIT, 69
    bcf     STATUS, C_BIT, a
    bcf     STATUS, DC_BIT, a
    movlw   0xCE
    daw
    CHKW    0x34, 70
    CHKSET  STATUS, C_BIT, 71
    bcf     STATUS, C_BIT, a
    bcf     STATUS, DC_BIT, a
    movlw   0x15
    daw
    CHKW    0x15, 72
    CHKCLR  STATUS, C_BIT, 73
; TO and PD are read-only and set after power-on: writing 0 leaves them
    movlw   0
    movwf   STATUS, a
    movf    STATUS, w, a
    CHKW    0x60, 74
; MULLW and MULWF leave the flags alone
    movlw   0x1F
    movwf   STATUS, a
    movlw   0x00
    mullw   0x00
    CHKST   0x1F, 75

    TESTEND
    END
