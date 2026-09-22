; Indirect addressing: LFSR, INDF/POSTINC/POSTDEC/PREINC/PLUSW on all three
; FSRs (PLUSW with a negative W), ADDFSR/SUBFSR, 14-bit wrap, and FSRs that
; point into the core registers.
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
    lfsr    0, 0x0500
    lfsr    1, 0x1234
    lfsr    2, 0x3FFF
    movf    FSR0L, w, a
    CHKW    0x00, 1
    movf    FSR0H, w, a
    CHKW    0x05, 2
    movf    FSR1L, w, a
    CHKW    0x34, 3
    movf    FSR1H, w, a
    CHKW    0x12, 4
    movf    FSR2L, w, a
    CHKW    0xFF, 5
    movf    FSR2H, w, a
    CHKW    0x3F, 6
; INDF0 writes 0x500 (SAVE)
    movlw   0xA1
    movwf   INDF0, a
    CHKF    SAVE, 0xA1, 7
; POSTINC0 writes then steps
    movlw   0xA2
    movwf   POSTINC0, a
    CHKF    SAVE, 0xA2, 8
    movf    FSR0L, w, a
    CHKW    1, 9
; PREINC0 steps then writes 0x502 (RESULT)
    movlw   0xA3
    movwf   PREINC0, a
    CHKF    RESULT, 0xA3, 10
    movf    FSR0L, w, a
    CHKW    2, 11
; POSTDEC0 reads 0x502 then steps back
    movf    POSTDEC0, w, a
    CHKW    0xA3, 12
    movf    FSR0L, w, a
    CHKW    1, 13
; PLUSW0 with W = -1 and W = +1; FSR0 is left alone
    movlw   0xFF
    movf    PLUSW0, w, a
    CHKW    0xA2, 14
    movlw   1
    movf    PLUSW0, w, a
    CHKW    0xA3, 15
    movf    FSR0L, w, a
    CHKW    1, 16
; INDF1 writes where a banked access reads
    movlw   0xB1
    movwf   INDF1, a
    movlb   0x12
    movf    0x34, w, b
    CHKW    0xB1, 17
    movlb   0
; the FSRs are 14 bits wide and wrap
    lfsr    1, 0
    movf    POSTDEC1, w, a
    movf    FSR1H, w, a
    CHKW    0x3F, 18
    movf    FSR1L, w, a
    CHKW    0xFF, 19
    movf    PREINC1, w, a
    movf    FSR1H, w, a
    CHKW    0, 20
    movf    FSR1L, w, a
    CHKW    0, 21
; PLUSW2 as a store with a negative offset: 0x503 - 2 = 0x501
    lfsr    2, COUNT_ABS
    movlw   0xFE
    movwf   PLUSW2, a
    CHKF    SCRATCH, 0xFE, 22
; ADDFSR and SUBFSR
    lfsr    0, 0x0100
    addfsr  0, 0x3F
    movf    FSR0L, w, a
    CHKW    0x3F, 23
    movf    FSR0H, w, a
    CHKW    1, 24
    subfsr  0, 0x3F
    movf    FSR0L, w, a
    CHKW    0x00, 25
    subfsr  0, 1
    movf    FSR0L, w, a
    CHKW    0xFF, 26
    movf    FSR0H, w, a
    CHKW    0, 41
    addfsr  2, 1
    movf    FSR2L, w, a
    CHKW    4, 27
    subfsr  1, 1
    movf    FSR1H, w, a
    CHKW    0x3F, 28
; an FSR pointing into the core registers reaches them
    lfsr    0, WREG
    movlw   0x5A
    movf    INDF0, w, a
    CHKW    0x5A, 29
; ...except the indirect operands themselves, which read as zero
    lfsr    0, INDF1
    movlw   0xEE
    movf    INDF0, w, a
    CHKW    0, 30
    lfsr    0, BSR
    movlw   0x21
    movwf   INDF0, a
    movf    BSR, w, a
    CHKW    0x21, 31
    clrf    BSR, a
    lfsr    0, FSR1L
    movlw   0x77
    movwf   INDF0, a
    movf    FSR1L, w, a
    CHKW    0x77, 32
; FSRnH keeps six bits
    movlw   0xFF
    movwf   FSR2H, a
    movf    FSR2H, w, a
    CHKW    0x3F, 33
; STATUS through an FSR is read before MOVF updates Z and N
    lfsr    0, STATUS
    movlw   0x1F
    movwf   STATUS, a
    movf    INDF0, w, a
    CHKW    0x7F, 34
; MOVF INDF0, F writes back and sets Z
    lfsr    0, SAVE_ABS
    clrf    SAVE, a
    bcf     STATUS, Z_BIT, a
    movf    INDF0, f, a
    CHKSET  STATUS, Z_BIT, 35
; PCL through an FSR reads the next address and latches PCLATH
    lfsr    0, PCL
    clrf    PCLATH, a
    movf    INDF0, w, a
ind_pcl:
    CHKW    low ind_pcl, 36
    movf    PCLATH, w, a
    CHKW    high ind_pcl, 37
; a PCL write through an FSR is a computed jump
    movlw   high ind_target
    movwf   PCLATH, a
    clrf    PCLATU, a
    movlw   low ind_target
    movwf   INDF0, a
    movlw   38
    goto    _fail
ind_target:
; POSTINC aimed at a core register still steps the FSR
    lfsr    0, PRODL
    movlw   0x99
    movwf   POSTINC0, a
    movf    PRODL, w, a
    CHKW    0x99, 39
    movf    FSR0L, w, a
    CHKW    low PRODH, 40
    TESTEND
    END
