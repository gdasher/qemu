; Data memory addressing: the access bank (a = 0, f < 0x60 is 0x500 + f,
; f >= 0x60 is 0x400 + f), banked addressing through BSR, a banked access
; that lands in the core register window (BSR = 4), and the BSR mask.
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
; a = 0, f < 0x60: Access RAM at 0x500 + f
    movlw   0xA5
    movwf   0x10, a
    movlb   5
    movf    0x10, w, b
    CHKW    0xA5, 1
    movlw   0x5A
    movwf   0x5F, a
    movf    0x5F, w, b
    CHKW    0x5A, 2
; a = 0, f >= 0x60: the SFR half at 0x400 + f
    movlw   0x3C
    movwf   0x60, a
    movlb   4
    movf    0x60, w, b
    CHKW    0x3C, 3
    movlw   0xC3
    movwf   0xD5, a
    movf    0xD5, w, b
    CHKW    0xC3, 4
; bank 5 offset 0x60 is not the access bank's 0x60
    movlb   5
    movlw   0x11
    movwf   0x60, b
    movf    0x60, w, a
    CHKW    0x3C, 5
; banked into the core window: BSR = 4, f = 0xE8 is WREG
    movlb   4
    movlw   0x42
    movf    0xE8, w, b
    CHKW    0x42, 6
    movlw   0x2B
    movwf   0xE0, b
    movf    BSR, w, a
    CHKW    0x2B, 7
; the same offsets in another bank are plain RAM
    movlb   5
    movlw   0x77
    movwf   0xE8, b
    movlw   0x00
    movf    0xE8, w, b
    CHKW    0x77, 8
    movlb   6
    movf    0xE8, w, b
    CHKW    0, 9
; BSR keeps six bits
    movlw   0xFF
    movwf   BSR, a
    movf    BSR, w, a
    CHKW    0x3F, 10
    clrf    BSR, a
; MOVFF sees the same bytes at their full addresses
    movlw   0x99
    movwf   SCRATCH, a
    movff   SCRATCH_ABS, RESULT_ABS
    CHKF    RESULT, 0x99, 11
; STATUS through a banked write
    movlb   4
    movlw   0x1F
    movwf   0xD8, b
    CHKST   0x1F, 12
; INDF0 through a banked read
    lfsr    0, 0x510
    movlw   0x5C
    movwf   0x10, a
    movlb   4
    movf    0xEF, w, b
    CHKW    0x5C, 13
; a banked read-modify-write on a core register
    movlb   4
    movlw   0x10
    movwf   FSR2L, a
    incf    0xD9, f, b
    movf    FSR2L, w, a
    CHKW    0x11, 14
    TESTEND
    END
