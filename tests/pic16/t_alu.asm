; Arithmetic, logic, rotates and shifts, with their STATUS effects.
;
; The result is stashed with MOVWF (which touches no flags) before the flag
; checks, because CHKF and CHKW both disturb Z.
;
; SPDX-License-Identifier: GPL-2.0-or-later

    processor 16f1829
    radix dec
    include "pictest.inc"

    org 0
    goto    main

    org 8
main:
;-------------------------------------------------------------------- ADDWF
; 0x0F + 0x01: carry out of bit 3 but not bit 7.
    movlw   0x0F
    movwf   SCRATCH
    movlw   0x01
    addwf   SCRATCH, 0
    movwf   RESULT
    CHKCLR  STATUS, C, 1
    CHKSET  STATUS, DC, 2
    CHKCLR  STATUS, Z, 3
    CHKF    RESULT, 0x10, 4

; 0xFF + 0x01 wraps to zero, setting all three.
    movlw   0xFF
    movwf   SCRATCH
    movlw   0x01
    addwf   SCRATCH, 0
    movwf   RESULT
    CHKSET  STATUS, C, 5
    CHKSET  STATUS, DC, 6
    CHKSET  STATUS, Z, 7
    CHKF    RESULT, 0x00, 8

;-------------------------------------------------------------------- ADDLW
; 0x80 + 0x80: carry out of bit 7 only.
    movlw   0x80
    addlw   0x80
    movwf   RESULT
    CHKSET  STATUS, C, 9
    CHKCLR  STATUS, DC, 10
    CHKSET  STATUS, Z, 11
    CHKF    RESULT, 0x00, 12

;-------------------------------------------------------------------- SUBWF
; Borrow has inverted polarity: C set means no borrow (DS40002637A 9.7.4).
; 0x10 - 0x01 borrows from the high nibble, so DC is clear.
    movlw   0x10
    movwf   SCRATCH
    movlw   0x01
    subwf   SCRATCH, 0
    movwf   RESULT
    CHKSET  STATUS, C, 13
    CHKCLR  STATUS, DC, 14
    CHKCLR  STATUS, Z, 15
    CHKF    RESULT, 0x0F, 16

; Equal operands: no borrow anywhere, result zero.
    movlw   0x05
    movwf   SCRATCH
    movlw   0x05
    subwf   SCRATCH, 0
    movwf   RESULT
    CHKSET  STATUS, C, 17
    CHKSET  STATUS, DC, 18
    CHKSET  STATUS, Z, 19
    CHKF    RESULT, 0x00, 20

; 0x01 - 0x02 borrows, clearing C.
    movlw   0x01
    movwf   SCRATCH
    movlw   0x02
    subwf   SCRATCH, 0
    movwf   RESULT
    CHKCLR  STATUS, C, 21
    CHKF    RESULT, 0xFF, 22

;-------------------------------------------------------------------- SUBLW
; k - W, not W - k.
    movlw   0x01
    sublw   0x10
    movwf   RESULT
    CHKSET  STATUS, C, 23
    CHKF    RESULT, 0x0F, 24

;------------------------------------------------------------------- ADDWFC
    bsf     STATUS, C
    movlw   0x0F
    movwf   SCRATCH
    movlw   0x01
    addwfc  SCRATCH, 0
    movwf   RESULT
    CHKCLR  STATUS, C, 25
    CHKSET  STATUS, DC, 26
    CHKF    RESULT, 0x11, 27

;------------------------------------------------------------------- SUBWFB
; f - W - !C, so C set behaves like plain SUBWF.
    bsf     STATUS, C
    movlw   0x10
    movwf   SCRATCH
    movlw   0x01
    subwfb  SCRATCH, 0
    movwf   RESULT
    CHKSET  STATUS, C, 28
    CHKF    RESULT, 0x0F, 29

    bcf     STATUS, C
    movlw   0x10
    movwf   SCRATCH
    movlw   0x01
    subwfb  SCRATCH, 0
    movwf   RESULT
    CHKSET  STATUS, C, 30
    CHKF    RESULT, 0x0E, 31

;--------------------------------------------------------------- ANDWF etc.
    movlw   0xF0
    movwf   SCRATCH
    movlw   0x0F
    andwf   SCRATCH, 0
    movwf   RESULT
    CHKSET  STATUS, Z, 32
    CHKF    RESULT, 0x00, 33

    movlw   0xF0
    movwf   SCRATCH
    movlw   0x0F
    iorwf   SCRATCH, 0
    movwf   RESULT
    CHKCLR  STATUS, Z, 34
    CHKF    RESULT, 0xFF, 35

    movlw   0xFF
    movwf   SCRATCH
    movlw   0xFF
    xorwf   SCRATCH, 0
    movwf   RESULT
    CHKSET  STATUS, Z, 36
    CHKF    RESULT, 0x00, 37

;--------------------------------------------------------- COMF, INCF, DECF
    movlw   0x0F
    movwf   SCRATCH
    comf    SCRATCH, 0
    movwf   RESULT
    CHKCLR  STATUS, Z, 38
    CHKF    RESULT, 0xF0, 39

    movlw   0xFF
    movwf   SCRATCH
    incf    SCRATCH, 0
    movwf   RESULT
    CHKSET  STATUS, Z, 40
    CHKF    RESULT, 0x00, 41

    movlw   0x01
    movwf   SCRATCH
    decf    SCRATCH, 0
    movwf   RESULT
    CHKSET  STATUS, Z, 42
    CHKF    RESULT, 0x00, 43

;-------------------------------------------------------------------- SWAPF
; Affects no flags, so Z must survive from the DECF above.
    movlw   0x12
    movwf   SCRATCH
    swapf   SCRATCH, 0
    movwf   RESULT
    CHKSET  STATUS, Z, 44
    CHKF    RESULT, 0x21, 45

;---------------------------------------------------------------- RLF / RRF
; Rotate through carry; neither affects Z.
    bcf     STATUS, C
    movlw   0x80
    movwf   SCRATCH
    rlf     SCRATCH, 0
    movwf   RESULT
    CHKSET  STATUS, C, 46
    CHKF    RESULT, 0x00, 47

    bsf     STATUS, C
    movlw   0x01
    movwf   SCRATCH
    rlf     SCRATCH, 0
    movwf   RESULT
    CHKCLR  STATUS, C, 48
    CHKF    RESULT, 0x03, 49

    bcf     STATUS, C
    movlw   0x01
    movwf   SCRATCH
    rrf     SCRATCH, 0
    movwf   RESULT
    CHKSET  STATUS, C, 50
    CHKF    RESULT, 0x00, 51

    bsf     STATUS, C
    movlw   0x80
    movwf   SCRATCH
    rrf     SCRATCH, 0
    movwf   RESULT
    CHKCLR  STATUS, C, 52
    CHKF    RESULT, 0xC0, 53

;-------------------------------------------------------- LSLF, LSRF, ASRF
    movlw   0x81
    movwf   SCRATCH
    lslf    SCRATCH, 0
    movwf   RESULT
    CHKSET  STATUS, C, 54
    CHKCLR  STATUS, Z, 55
    CHKF    RESULT, 0x02, 56

    movlw   0x81
    movwf   SCRATCH
    lsrf    SCRATCH, 0
    movwf   RESULT
    CHKSET  STATUS, C, 57
    CHKF    RESULT, 0x40, 58

; Arithmetic shift keeps the sign bit.
    movlw   0x81
    movwf   SCRATCH
    asrf    SCRATCH, 0
    movwf   RESULT
    CHKSET  STATUS, C, 59
    CHKF    RESULT, 0xC0, 60

    movlw   0x01
    movwf   SCRATCH
    asrf    SCRATCH, 0
    movwf   RESULT
    CHKSET  STATUS, C, 61
    CHKSET  STATUS, Z, 62
    CHKF    RESULT, 0x00, 63

;----------------------------------------------------- destination selector
; d = 1 writes back to f and leaves W alone.
    movlw   0x20
    movwf   SCRATCH
    movlw   0x01
    addwf   SCRATCH, 1
    CHKW    0x01, 64
    CHKF    SCRATCH, 0x21, 65

;--------------------------------------------------------------- MOVF and Z
    clrf    SCRATCH
    movf    SCRATCH, 1
    CHKSET  STATUS, Z, 66
    movlw   0x01
    movwf   SCRATCH
    movf    SCRATCH, 1
    CHKCLR  STATUS, Z, 67

;--------------------------------------------------------------- CLRF sets Z
    bcf     STATUS, Z
    clrf    SCRATCH
    CHKSET  STATUS, Z, 68
    CHKF    SCRATCH, 0x00, 69

    goto    _pass

    TESTEND
    end
