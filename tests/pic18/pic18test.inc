; Shared helpers for PIC18 instruction fixtures, assembled with XC8's pic-as
; (-mcpu=18f27q43). Core register addresses are the PIC18F-Q43's.
;
; A fixture exits through the test machine's device: writing EXITC terminates
; QEMU with that value, so 0 means pass and any other value identifies the
; check that failed. Check codes therefore start at 1.
;
; SPDX-License-Identifier: GPL-2.0-or-later

    PROCESSOR 18F27Q43
    RADIX DEC

; Core registers (bank 4, reachable with the access bit clear).
INTCON0  EQU 0x4D6
INTCON1  EQU 0x4D7
STATUS   EQU 0x4D8
FSR2L    EQU 0x4D9
FSR2H    EQU 0x4DA
PLUSW2   EQU 0x4DB
PREINC2  EQU 0x4DC
POSTDEC2 EQU 0x4DD
POSTINC2 EQU 0x4DE
INDF2    EQU 0x4DF
BSR      EQU 0x4E0
FSR1L    EQU 0x4E1
FSR1H    EQU 0x4E2
PLUSW1   EQU 0x4E3
PREINC1  EQU 0x4E4
POSTDEC1 EQU 0x4E5
POSTINC1 EQU 0x4E6
INDF1    EQU 0x4E7
WREG     EQU 0x4E8
FSR0L    EQU 0x4E9
FSR0H    EQU 0x4EA
PLUSW0   EQU 0x4EB
PREINC0  EQU 0x4EC
POSTDEC0 EQU 0x4ED
POSTINC0 EQU 0x4EE
INDF0    EQU 0x4EF
PCON0    EQU 0x4F0
PCON1    EQU 0x4F1
CPUDOZE  EQU 0x4F2
PRODL    EQU 0x4F3
PRODH    EQU 0x4F4
TABLAT   EQU 0x4F5
TBLPTRL  EQU 0x4F6
TBLPTRH  EQU 0x4F7
TBLPTRU  EQU 0x4F8
PCL      EQU 0x4F9
PCLATH   EQU 0x4FA
PCLATU   EQU 0x4FB
STKPTR   EQU 0x4FC
TOSL     EQU 0x4FD
TOSH     EQU 0x4FE
TOSU     EQU 0x4FF

; STATUS bits.
C_BIT    EQU 0
DC_BIT   EQU 1
Z_BIT    EQU 2
OV_BIT   EQU 3
N_BIT    EQU 4
PD_BIT   EQU 5
TO_BIT   EQU 6

; INTCON0 bits.
GIE      EQU 7
GIEL     EQU 6
IPEN     EQU 5

; Test device, in bank 1 of the SFR space. Reached with MOVFF (12-bit
; address) so no bank selection is needed.
PUTC     EQU 0x100
EXITC    EQU 0x101
DUMP     EQU 0x102
IRQH     EQU 0x103
IRQL     EQU 0x104

; Access RAM scratch (a = 0 addresses 0x500 + f).
SAVE     EQU 0x00
SCRATCH  EQU 0x01
RESULT   EQU 0x02
COUNT    EQU 0x03
; The same bytes as full 12-bit addresses, for MOVFF / MOVFFL / LFSR operands.
SAVE_ABS    EQU 0x500
SCRATCH_ABS EQU 0x501
RESULT_ABS  EQU 0x502
COUNT_ABS   EQU 0x503

; Fail with 'code' unless W equals 'expected'. MOVLW does not touch STATUS,
; so it is safe between the compare and the test.
CHKW MACRO expected, code
    xorlw   expected
    movlw   code
    btfss   STATUS, Z_BIT, a
    goto    _fail
    ENDM

; Fail with 'code' unless bit 'bit' of access-bank 'reg' is set / clear.
CHKSET MACRO reg, bit, code
    movlw   code
    btfss   reg, bit, a
    goto    _fail
    ENDM

CHKCLR MACRO reg, bit, code
    movlw   code
    btfsc   reg, bit, a
    goto    _fail
    ENDM

; Fail with 'code' unless the access-bank byte at 'reg' equals 'expected'.
CHKF MACRO reg, expected, code
    movf    reg, w, a
    xorlw   expected
    movlw   code
    btfss   STATUS, Z_BIT, a
    goto    _fail
    ENDM

; Fail with 'code' unless the flags (STATUS bits 4:0) equal 'expected'.
; Clobbers W and Z/N, so read the result out first.
CHKST MACRO expected, code
    movf    STATUS, w, a
    andlw   0x1F
    xorlw   expected
    movlw   code
    btfss   STATUS, Z_BIT, a
    goto    _fail
    ENDM

; Check W and the flags together: W must be 'w', flags 'st'. Uses 'code'
; for the flags and 'code' + 1 for the value.
CHKWS MACRO val, st, code
    movwf   RESULT, a
    CHKST   st, code
    CHKF    RESULT, val, code + 1
    ENDM

TESTEND MACRO
_pass:
    movlw   0
    movff   WREG, EXITC
_fail:
    movff   WREG, EXITC
    bra     _fail
    ENDM
