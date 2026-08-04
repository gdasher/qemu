; The RESET instruction.
;
; Data memory survives a software reset while the CPU state does not, so a
; flag in common RAM distinguishes the two passes through the entry point.
;
; SPDX-License-Identifier: GPL-2.0-or-later

    processor 16f1829
    radix dec
    include "pictest.inc"

FLAG    equ 0x7F

    org 0
    goto    main

    org 8
main:
    movf    FLAG, 0
    btfss   STATUS, Z
    goto    _after_reset

;-------------------------------------------------------------- first pass
; Dirty the CPU state so the reset has something to clear, then reset.
    movlw   1
    movwf   FLAG
    movlw   0x2A
    movwf   SCRATCH
    movlw   31
    movwf   BSR
    movlw   0x7F
    movwf   PCLATH
    movlw   0xFF
    movwf   FSR0L
    movwf   FSR0H
    reset
    movlw   1                   ; not reached
    goto    _fail

;------------------------------------------------------------- second pass
_after_reset:
; The core registers are back to their reset values.
    CHKF    BSR, 0x00, 2
    CHKF    PCLATH, 0x00, 3
    CHKF    FSR0L, 0x00, 4
    CHKF    FSR0H, 0x00, 5

; TO and PD read 1 after a reset.
    CHKSET  STATUS, TO, 6
    CHKSET  STATUS, PD, 7

; Data memory is untouched.
    CHKF    SCRATCH, 0x2A, 8
    CHKF    FLAG, 0x01, 9

    goto    _pass

    TESTEND
    end
