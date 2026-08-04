; Bit set/clear and the four skip instructions.
;
; A skip must leave the following instruction unexecuted, so each case puts a
; GOTO in the skip slot: reaching it is the failure.
;
; SPDX-License-Identifier: GPL-2.0-or-later

    processor 16f1829
    radix dec
    include "pictest.inc"

    org 0
    goto    main

    org 8
main:
;---------------------------------------------------------------- BCF / BSF
    movlw   0xFF
    movwf   SCRATCH
    bcf     SCRATCH, 3
    CHKF    SCRATCH, 0xF7, 1

    clrf    SCRATCH
    bsf     SCRATCH, 5
    CHKF    SCRATCH, 0x20, 2

    bsf     SCRATCH, 0
    bsf     SCRATCH, 7
    CHKF    SCRATCH, 0xA1, 3

; Neither touches STATUS.
    bsf     STATUS, Z
    clrf    SCRATCH
    bsf     SCRATCH, 1
    CHKSET  STATUS, Z, 4

;------------------------------------------------------------------- BTFSC
; Skips when the bit is clear.
    clrf    SCRATCH
    btfsc   SCRATCH, 0
    goto    _fail_5
    movlw   0x01
    movwf   SCRATCH
    btfsc   SCRATCH, 0          ; bit set, no skip
    goto    _btfsc_ok
    movlw   6
    goto    _fail
_btfsc_ok:

;------------------------------------------------------------------- BTFSS
; Skips when the bit is set.
    movlw   0x80
    movwf   SCRATCH
    btfss   SCRATCH, 7
    goto    _fail_7
    clrf    SCRATCH
    btfss   SCRATCH, 7          ; bit clear, no skip
    goto    _btfss_ok
    movlw   8
    goto    _fail
_btfss_ok:

;------------------------------------------------------------------ DECFSZ
; Skips when the decremented result is zero, and writes the result back.
    movlw   1
    movwf   COUNT
    decfsz  COUNT, 1
    goto    _fail_9
    CHKF    COUNT, 0x00, 10

    movlw   2
    movwf   COUNT
    decfsz  COUNT, 1            ; 2 -> 1, no skip
    goto    _decfsz_ok
    movlw   11
    goto    _fail
_decfsz_ok:
    CHKF    COUNT, 0x01, 12

; d = 0 leaves the file register alone and puts the result in W.
    movlw   5
    movwf   COUNT
    decfsz  COUNT, 0
    goto    _decfsz_w
    movlw   13
    goto    _fail
_decfsz_w:
    CHKF    COUNT, 0x05, 14

;------------------------------------------------------------------ INCFSZ
    movlw   0xFF
    movwf   COUNT
    incfsz  COUNT, 1
    goto    _fail_15
    CHKF    COUNT, 0x00, 16

    movlw   0x01
    movwf   COUNT
    incfsz  COUNT, 1
    goto    _incfsz_ok
    movlw   17
    goto    _fail
_incfsz_ok:
    CHKF    COUNT, 0x02, 18

;--------------------------------------------------------- DECFSZ as a loop
; The usual countdown idiom, to check the skip repeats correctly.
    movlw   10
    movwf   COUNT
    clrf    SCRATCH
_loop:
    incf    SCRATCH, 1
    decfsz  COUNT, 1
    goto    _loop
    CHKF    SCRATCH, 10, 19
    CHKF    COUNT, 0x00, 20

    goto    _pass

_fail_5:
    movlw   5
    goto    _fail
_fail_7:
    movlw   7
    goto    _fail
_fail_9:
    movlw   9
    goto    _fail
_fail_15:
    movlw   15
    goto    _fail

    TESTEND
    end
