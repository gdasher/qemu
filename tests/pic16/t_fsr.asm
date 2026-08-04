; Indirect addressing: INDF, the MOVIW/MOVWI modes, ADDFSR, and the three
; regions the FSR address space covers -- banked data, the linear GPR window,
; and program memory.
;
; ADDFSR is emitted with dw because gpasm 1.4.0 rejects every spelling of its
; FSR operand.
;
; SPDX-License-Identifier: GPL-2.0-or-later

    processor 16f1829
    radix dec
    include "pictest.inc"

ADDFSR0 equ 0x3100              ; | k & 0x3F
ADDFSR1 equ 0x3140

    org 0
    goto    main

    org 8
main:
    clrf    BSR

;--------------------------------------------------------------- INDF basic
    clrf    FSR0H
    movlw   0x30
    movwf   FSR0L
    movlw   0xA5
    movwf   INDF0
    CHKF    0x30, 0xA5, 1

    movf    INDF0, 0
    movwf   RESULT
    CHKF    RESULT, 0xA5, 2

; Reading zero through INDF sets Z.
    clrf    0x31
    movlw   0x31
    movwf   FSR0L
    movf    INDF0, 0
    CHKSET  STATUS, Z, 3

;------------------------------------------------------- MOVIW / MOVWI modes
    movlw   0x11
    movwf   0x30
    movlw   0x22
    movwf   0x31
    movlw   0x33
    movwf   0x32

    movlw   0x30
    movwf   FSR0L
    moviw   FSR0++              ; read 0x30, then advance
    movwf   RESULT
    CHKF    RESULT, 0x11, 4
    CHKF    FSR0L, 0x31, 5

    moviw   ++FSR0              ; advance to 0x32, then read
    movwf   RESULT
    CHKF    RESULT, 0x33, 6
    CHKF    FSR0L, 0x32, 7

    moviw   --FSR0              ; back to 0x31, then read
    movwf   RESULT
    CHKF    RESULT, 0x22, 8
    CHKF    FSR0L, 0x31, 9

    moviw   FSR0--              ; read 0x31, then retreat
    movwf   RESULT
    CHKF    RESULT, 0x22, 10
    CHKF    FSR0L, 0x30, 11

; Indexed form leaves the FSR alone.
    moviw   2[FSR0]
    movwf   RESULT
    CHKF    RESULT, 0x33, 12
    CHKF    FSR0L, 0x30, 13

    movlw   0x5C
    movwi   1[FSR0]
    CHKF    0x31, 0x5C, 14
    CHKF    FSR0L, 0x30, 15

    movlw   0x6D
    movwi   FSR0++
    CHKF    0x30, 0x6D, 16
    CHKF    FSR0L, 0x31, 17

;-------------------------------------------------------------------- ADDFSR
    clrf    FSR0H
    movlw   0x30
    movwf   FSR0L
    dw      ADDFSR0 + 5
    CHKF    FSR0L, 0x35, 18

    dw      ADDFSR0 + (-3 & 0x3F)
    CHKF    FSR0L, 0x32, 19

    clrf    FSR1H
    movlw   0x40
    movwf   FSR1L
    dw      ADDFSR1 + 1
    CHKF    FSR1L, 0x41, 20

;------------------------------------------------- core registers through FSR
; The low twelve addresses of a bank are the core registers, which are not in
; memory; an FSR pointing at one must still reach it.
    clrf    FSR1H
    movlw   BSR
    movwf   FSR1L
    movlw   7
    movwf   INDF1               ; BSR = 7
    movf    BSR, 0
    movwf   RESULT
    clrf    BSR
    CHKF    RESULT, 7, 21

;----------------------------------------------------------- linear window
; 0x2000 upwards is a packed view of the 80-byte GPR block of each bank, so
; 0x2000 is bank 0 offset 0x20 and 0x2050 is bank 1 offset 0x20.
    movlw   0x20
    movwf   FSR0H
    clrf    FSR0L               ; FSR0 = 0x2000
    movlw   0xA1
    movwf   INDF0
    clrf    BSR
    CHKF    0x20, 0xA1, 22

    movlw   0x50
    movwf   FSR0L               ; FSR0 = 0x2050
    movlw   0xB2
    movwf   INDF0
    movlw   1
    movwf   BSR                 ; bank 1
    CHKF    0x20, 0xB2, 23
    clrf    BSR

;-------------------------------------------------------------- common RAM
; 0x70-0x7F is the same sixteen bytes in every bank. Direct addressing needs
; no BSR to reach it, but an FSR carries a bank number that has to be folded
; away: 0x0F70 and 0x1F70 must both land on 0x0070.
    movlw   0xC3
    movwf   SAVE                ; direct, address 0x70
    clrf    FSR0H
    movlw   0x70
    movwf   FSR0L               ; FSR0 = 0x0070
    movf    INDF0, 0
    movwf   RESULT
    CHKF    RESULT, 0xC3, 27

    movlw   0x0F
    movwf   FSR0H
    movlw   0x70
    movwf   FSR0L               ; FSR0 = 0x0F70, bank 30
    movf    INDF0, 0
    movwf   RESULT
    CHKF    RESULT, 0xC3, 28

    movlw   0x1F
    movwf   FSR0H               ; FSR0 = 0x1F70, bank 62
    movlw   0xD4
    movwf   INDF0
    CHKF    SAVE, 0xD4, 29

;------------------------------------------------------------ program memory
; With the top FSR bit set, a read returns the low byte of the program word.
    movlw   low _pfmdata
    movwf   FSR0L
    movlw   high _pfmdata
    iorlw   0x80
    movwf   FSR0H
    movf    INDF0, 0
    movwf   RESULT
    CHKF    RESULT, 0x34, 24

    moviw   1[FSR0]
    movwf   RESULT
    CHKF    RESULT, 0x78, 25

; Writes to program memory through an FSR are ignored.
    movlw   0xEE
    movwf   INDF0
    movf    INDF0, 0
    movwf   RESULT
    CHKF    RESULT, 0x34, 26

    goto    _pass

_pfmdata:
    dw      0x1234
    dw      0x0578

    TESTEND
    end
