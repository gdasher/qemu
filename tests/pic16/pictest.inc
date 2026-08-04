; Shared helpers for PIC16 instruction fixtures.
;
; Fixtures deliberately avoid MOVLB and BANKSEL. gputils emits the 5-bit MOVLB
; that older PIC16F1 parts use (0x0020-0x003F), while this family encodes it
; with a 6-bit literal at 0x0140-0x017F. BSR is core register 0x08 and is
; reachable from every bank, so bank selection goes through it instead.
;
; A fixture exits through the test machine's device: writing EXITC terminates
; QEMU with that value, so 0 means pass and any other value identifies the
; check that failed. Check codes therefore start at 1.
;
; SPDX-License-Identifier: GPL-2.0-or-later

; Core registers, addressable from any bank.
INDF0   equ 0x00
INDF1   equ 0x01
PCL     equ 0x02
STATUS  equ 0x03
FSR0L   equ 0x04
FSR0H   equ 0x05
FSR1L   equ 0x06
FSR1H   equ 0x07
BSR     equ 0x08
WREG    equ 0x09
PCLATH  equ 0x0A
INTCON  equ 0x0B

; STATUS bits.
C       equ 0
DC      equ 1
Z       equ 2
PD      equ 3
TO      equ 4

; Test device, in the SFR slots of bank 63.
TESTBANK equ 63
PUTC     equ 0x0C
EXITC    equ 0x0D
DUMP     equ 0x0E

; FSR numbers, for the indirect addressing mnemonics.
FSR0    equ 0
FSR1    equ 1

; Common RAM: the same sixteen bytes in every bank, so no BSR setup needed.
SAVE     equ 0x70
SCRATCH  equ 0x71
RESULT   equ 0x72
COUNT    equ 0x73

SETBANK macro n
    movlw   n
    movwf   BSR
    endm

; Fail with 'code' unless W equals 'expected'. MOVLW does not touch STATUS,
; so it is safe between the compare and the test.
CHKW macro expected, code
    xorlw   expected
    movlw   code
    btfss   STATUS, Z
    goto    _fail
    endm

CHKSET macro reg, bit, code
    movlw   code
    btfss   reg, bit
    goto    _fail
    endm

CHKCLR macro reg, bit, code
    movlw   code
    btfsc   reg, bit
    goto    _fail
    endm

; Fail with 'code' unless the byte at 'reg' equals 'expected'.
CHKF macro reg, expected, code
    movf    reg, 0
    xorlw   expected
    movlw   code
    btfss   STATUS, Z
    goto    _fail
    endm

TESTEND macro
_pass:
    SETBANK TESTBANK
    movlw   0
    movwf   EXITC
_fail:
    movwf   SAVE
    SETBANK TESTBANK
    movf    SAVE, 0
    movwf   EXITC
    endm
