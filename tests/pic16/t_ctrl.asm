; Calls, returns, the hardware stack, and the PCLATH-dependent jumps.
;
; SPDX-License-Identifier: GPL-2.0-or-later

    processor 16f1829
    radix dec
    include "pictest.inc"

    org 0
    goto    main

    org 8
main:
;------------------------------------------------------------ CALL / RETURN
    clrf    PCLATH
    clrf    SCRATCH
    call    _add_one
    call    _add_one
    call    _add_one
    CHKF    SCRATCH, 3, 1

;----------------------------------------------------------- nested returns
; Three deep, so the stack has to unwind in order.
    clrf    SCRATCH
    call    _lvl1
    CHKF    SCRATCH, 0x07, 2

;-------------------------------------------------------------------- RETLW
    call    _retlw_test
    CHKW    0x77, 3

;--------------------------------------------------------------------- BRA
; Forward.
    bra     _bra_fwd
    movlw   4
    goto    _fail
_bra_fwd:

; Backward, as a loop.
    clrf    COUNT
_bra_loop:
    incf    COUNT, 1
    movf    COUNT, 0
    xorlw   3
    btfss   STATUS, Z
    bra     _bra_loop
    CHKF    COUNT, 3, 5

;----------------------------------------------------- PCL write (computed)
; Writing PCL jumps to PCLATH:W.
    movlw   high _pcl_target
    movwf   PCLATH
    movlw   low _pcl_target
    movwf   PCL
    movlw   6
    goto    _fail
_pcl_target:
    clrf    PCLATH

;--------------------------------------------------- GOTO across a 2K page
; GOTO supplies PC[10:0]; PCLATH[6:3] supplies PC[14:11]. _far sits at 0x900,
; so PC[14:11] must be 1, which means PCLATH bit 3.
    movlw   0x08
    movwf   PCLATH
    goto    _far
_far_return:
    clrf    PCLATH
    CHKF    SCRATCH, 0x2A, 7

;--------------------------------------------------------- stack wrap depth
; Sixteen entries: fill and unwind them all.
    clrf    COUNT
    call    _deep
    CHKF    COUNT, 8, 8

    goto    _pass

;------------------------------------------------------------- subroutines
_add_one:
    incf    SCRATCH, 1
    return

_lvl1:
    bsf     SCRATCH, 0
    call    _lvl2
    return
_lvl2:
    bsf     SCRATCH, 1
    call    _lvl3
    return
_lvl3:
    bsf     SCRATCH, 2
    return

_retlw_test:
    retlw   0x77

; Eight nested calls, unwinding back through every frame.
_deep:
    incf    COUNT, 1
    movf    COUNT, 0
    xorlw   8
    btfsc   STATUS, Z
    return
    call    _deep
    return

    TESTEND

    org 0x900
_far:
    movlw   0x2A
    movwf   SCRATCH
    clrf    PCLATH
    goto    _far_return

    end
