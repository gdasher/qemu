; Interrupt entry and exit: the single vector at 0x0004, GIE gating, and the
; automatic context save into the shadow registers.
;
; The test device's IRQ register stands in for a peripheral raising a request.
; It is reached through FSR1 rather than by selecting bank 63, so that BSR
; stays part of the context under test.
;
; SPDX-License-Identifier: GPL-2.0-or-later

    processor 16f1829
    radix dec
    include "pictest.inc"

IRQREG  equ 0x1F8F              ; bank 63, test device offset 3
ISRFLAG equ 0x7C                ; common RAM, so the ISR can reach it
GIEBAD  equ 0x7D
COUNTER equ 0x7E

    org 0
    goto    main

;--------------------------------------------------------------------- ISR
    org 4
isr:
; GIE must have been cleared on entry, or a second request could re-enter.
    movlw   1
    btfsc   INTCON, 7
    movwf   GIEBAD

; Drop the request before returning, otherwise RETFIE re-enables GIE and the
; handler runs again immediately.
    movlw   0
    movwf   INDF1

    incf    COUNTER, 1
    movlw   1
    movwf   ISRFLAG

; Trash everything the hardware shadows. RETFIE must undo all of it.
    movlw   0x3F
    movwf   BSR
    movlw   0x7F
    movwf   PCLATH
    movlw   0xFF
    movwf   FSR0L
    movwf   FSR0H
    bsf     STATUS, C
    bsf     STATUS, Z
    movlw   0x12
    retfie

;-------------------------------------------------------------------- main
    org 0x20
main:
    clrf    BSR
    clrf    ISRFLAG
    clrf    GIEBAD
    clrf    COUNTER

; FSR1 addresses the IRQ register for the rest of the test.
    movlw   low IRQREG
    movwf   FSR1L
    movlw   high IRQREG
    movwf   FSR1H

;---------------------------------------------- a request with GIE clear waits
    bcf     INTCON, 7
    bsf     INTCON, 6           ; PEIE: gates every source on this family
    movlw   1
    movwf   INDF1               ; raise the request

    movlw   20                  ; give it plenty of chances to fire
    movwf   COUNT
_settle:
    decfsz  COUNT, 1
    goto    _settle
    CHKF    ISRFLAG, 0, 1

;------------------------------------------------------- context under test
    movlw   3
    movwf   BSR
; PCLATH bits 6:3 pick the GOTO page, so only the low bits are safe to dirty.
    movlw   0x07
    movwf   PCLATH
    movlw   0x11
    movwf   FSR0L
    movlw   0x22
    movwf   FSR0H
    bcf     STATUS, C
    movlw   0xA5

;--------------------------------------------------------- enabling GIE fires
    bsf     INTCON, 7
_wait:
    movf    ISRFLAG, 1          ; MOVF with d=1 leaves W alone
    btfsc   STATUS, Z
    goto    _wait

; W came back from the shadow register, not from the ISR.
    movwf   RESULT
    CHKF    RESULT, 0xA5, 2
    CHKF    BSR, 3, 3
    CHKF    PCLATH, 0x07, 4
    CHKF    FSR0L, 0x11, 5
    CHKF    FSR0H, 0x22, 6
    CHKF    GIEBAD, 0, 7
    CHKF    COUNTER, 1, 8

; RETFIE sets GIE again.
    CHKSET  INTCON, 7, 9

;---------------------------------------------------------- and again, once
; A second request must be taken, proving the first exit left a usable state.
    clrf    ISRFLAG
    movlw   1
    movwf   INDF1
_wait2:
    movf    ISRFLAG, 1
    btfsc   STATUS, Z
    goto    _wait2
    CHKF    COUNTER, 2, 10

;--------------------------------------------------------------- stack depth
; Entry pushed and RETFIE popped, so the stack is back where it started.
    CHKF    ISRFLAG, 1, 11

    goto    _pass

    TESTEND
    end
