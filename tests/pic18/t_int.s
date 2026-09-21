; Interrupts: both legacy vectors, IPEN clear and set, GIE/GIEL gating, the
; INTCON1 STAT state, a high-priority request preempting a low-priority
; handler, and the automatic context save restored by RETFIE 1.
;
; The test device's IRQH/IRQL registers stand in for the SoC's two request
; lines. A request raised by a store is taken before the next instruction,
; as on hardware; the BRA after each raise only makes that boundary visible.
;
; SPDX-License-Identifier: GPL-2.0-or-later

    include "pic18test.inc"
    CONFIG MVECEN = OFF
    CONFIG STVREN = OFF

HFLAG     EQU 0x10
LFLAG     EQU 0x11
STATH     EQU 0x12
STATL     EQU 0x13
GIESEEN   EQU 0x14
NEST      EQU 0x15
NESTOK    EQU 0x16
STATH_ABS EQU 0x512
STATL_ABS EQU 0x513

    PSECT resetvec,abs,class=CODE,reloc=2
    ORG 0
    goto    main

    PSECT hivec,abs,class=CODE,reloc=2
    ORG 8
    goto    hisr

    PSECT lovec,abs,class=CODE,reloc=2
    ORG 0x18
    goto    lisr

    PSECT text,abs,class=CODE,reloc=2
    ORG 0x100
main:
    clrf    HFLAG, a
    clrf    LFLAG, a
    clrf    STATH, a
    clrf    STATL, a
    clrf    GIESEEN, a
    clrf    NEST, a
    clrf    NESTOK, a
; a request with GIE clear waits
    bcf     INTCON0, GIE, a
    movlw   1
    movff   WREG, IRQH
    movlw   20
    movwf   COUNT, a
settle1:
    decfsz  COUNT, f, a
    bra     settle1
    CHKF    HFLAG, 0, 1
; set up a context and enable GIE: the handler runs at once and must
; restore all of it on the way out
    movlw   0x2A
    movwf   BSR, a
    lfsr    0, 0x1111
    lfsr    1, 0x2222
    lfsr    2, 0x3333
    movlw   0x33
    movwf   PRODL, a
    movlw   0x44
    movwf   PRODH, a
    movlw   0x05
    movwf   PCLATH, a
    movlw   0x03
    movwf   PCLATU, a
    movlw   0x1F
    movwf   STATUS, a
    movlw   0x77
    bsf     INTCON0, GIE, a
    nop
    nop
    movwf   RESULT, a
    CHKST   0x1F, 2
    CHKF    RESULT, 0x77, 3
    movf    BSR, w, a
    CHKW    0x2A, 4
    movf    FSR0H, w, a
    CHKW    0x11, 5
    movf    FSR0L, w, a
    CHKW    0x11, 6
    movf    FSR1H, w, a
    CHKW    0x22, 7
    movf    FSR2H, w, a
    CHKW    0x33, 8
    movf    PRODL, w, a
    CHKW    0x33, 9
    movf    PRODH, w, a
    CHKW    0x44, 10
    movf    PCLATH, w, a
    CHKW    0x05, 11
    movf    PCLATU, w, a
    CHKW    0x03, 12
    clrf    PCLATH, a
    clrf    PCLATU, a
    clrf    BSR, a
    CHKF    HFLAG, 1, 13
; the hardware leaves GIE set inside the handler; STAT said "high from main"
    CHKF    GIESEEN, 1, 14
    CHKF    STATH, 0x80, 15
    movf    INTCON1, w, a
    CHKW    0, 16
; with IPEN clear the low line also vectors to 0x0008
    movlw   1
    movff   WREG, IRQL
    bra     sync1
sync1:
    nop
    CHKF    HFLAG, 2, 17
    CHKF    LFLAG, 0, 18
; IPEN set, GIEL clear: a low request waits
    bsf     INTCON0, IPEN, a
    bcf     INTCON0, GIEL, a
    movlw   1
    movff   WREG, IRQL
    movlw   20
    movwf   COUNT, a
settle2:
    decfsz  COUNT, f, a
    bra     settle2
    CHKF    LFLAG, 0, 19
; GIEL set: the low handler runs through 0x0018
    bsf     INTCON0, GIEL, a
    nop
    nop
    CHKF    LFLAG, 1, 20
    CHKF    STATL, 0x40, 21
    CHKF    HFLAG, 2, 22
; a high request raised inside the low handler preempts it
    movlw   1
    movwf   NEST, a
    movlw   1
    movff   WREG, IRQL
    bra     sync2
sync2:
    nop
    nop
    CHKF    LFLAG, 2, 23
    CHKF    HFLAG, 3, 24
    CHKF    STATH, 0xC0, 25
    CHKF    NESTOK, 1, 26
    movf    INTCON1, w, a
    CHKW    0, 27
; and main's context survived the nested pair (both handlers trash BSR)
    movf    BSR, w, a
    CHKW    0, 28
    TESTEND

; High-priority handler: record the state, drop both lines, trash everything
; the hardware shadows, return with the fast restore.
hisr:
    incf    HFLAG, f, a
    movff   INTCON1, STATH_ABS
    movlw   1
    btfss   INTCON0, GIE, a
    movlw   0
    movwf   GIESEEN, a
    movlw   0
    movff   WREG, IRQH
    movff   WREG, IRQL
    movlw   0x3F
    movwf   BSR, a
    setf    FSR0L, a
    setf    FSR0H, a
    setf    FSR1L, a
    setf    FSR2H, a
    clrf    PRODL, a
    clrf    PRODH, a
    setf    PCLATH, a
    setf    PCLATU, a
    clrf    STATUS, a
    movlw   0x12
    retfie  f

; Low-priority handler: when NEST is set, raise the high line and expect the
; high handler to have run before this one continues.
lisr:
    incf    LFLAG, f, a
    movff   INTCON1, STATL_ABS
    movlw   0
    movff   WREG, IRQL
    movf    NEST, w, a
    bz      lisr_done
    clrf    NEST, a
    movf    HFLAG, w, a
    movwf   SAVE, a
    movlw   1
    movff   WREG, IRQH
    bra     lisr_sync
lisr_sync:
    nop
    nop
    incf    SAVE, w, a
    cpfseq  HFLAG, a
    bra     lisr_done
    movlw   1
    movwf   NESTOK, a
lisr_done:
    movlw   0x15
    movwf   BSR, a
    setf    FSR1L, a
    clrf    STATUS, a
    retfie  f
    END
