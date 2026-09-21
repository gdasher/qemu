; Skips: every skip instruction, over one-, two- and three-word instructions.
; A skipped multi-word instruction leaves its continuation words to execute
; as NOPs, so the whole thing is stepped over.
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
    clrf    SCRATCH, a
; BTFSC skips a one-word instruction when the bit is clear
    movlw   1
    btfsc   SCRATCH, 0, a
    movlw   2
    CHKW    1, 1
; BTFSS does not
    movlw   1
    btfss   SCRATCH, 0, a
    movlw   2
    CHKW    2, 2
; skip a two-word GOTO
    movlw   3
    btfsc   SCRATCH, 0, a
    goto    _fail
    CHKW    3, 3
; skip a two-word MOVFF
    movlw   4
    btfsc   SCRATCH, 0, a
    movff   WREG, SCRATCH_ABS
    CHKF    SCRATCH, 0, 4
; skip a three-word MOVFFL
    movlw   5
    btfsc   SCRATCH, 0, a
    movffl  WREG, SCRATCH_ABS
    CHKF    SCRATCH, 0, 5
; and the same two when not skipped
    movlw   6
    btfss   SCRATCH, 0, a
    movff   WREG, SCRATCH_ABS
    CHKF    SCRATCH, 6, 6
    movlw   7
    btfss   SCRATCH, 7, a
    movffl  WREG, SCRATCH_ABS
    CHKF    SCRATCH, 7, 7
; CPFSEQ: skip when f == W
    movlw   7
    cpfseq  SCRATCH, a
    movlw   0xEE
    CHKW    7, 8
    movlw   8
    cpfseq  SCRATCH, a
    movlw   9
    CHKW    9, 9
; CPFSGT: skip when f > W, unsigned
    movlw   6
    cpfsgt  SCRATCH, a
    movlw   0xEE
    CHKW    6, 10
    movlw   7
    cpfsgt  SCRATCH, a
    movlw   10
    CHKW    10, 11
    movlw   0x80
    movwf   SCRATCH, a
    movlw   0x7F
    cpfsgt  SCRATCH, a
    movlw   0xEE
    CHKW    0x7F, 12
; CPFSLT: skip when f < W, unsigned
    movlw   0x81
    cpfslt  SCRATCH, a
    movlw   0xEE
    CHKW    0x81, 13
    movlw   0x80
    cpfslt  SCRATCH, a
    movlw   14
    CHKW    14, 14
; DECFSZ
    movlw   2
    movwf   SCRATCH, a
    movlw   15
    decfsz  SCRATCH, f, a
    movlw   16
    CHKW    16, 15
    movlw   16
    decfsz  SCRATCH, f, a
    movlw   0xEE
    CHKW    16, 16
    CHKF    SCRATCH, 0, 17
; DCFSNZ: 0 -> 0xFF is not zero, so skip
    movlw   18
    dcfsnz  SCRATCH, f, a
    movlw   0xEE
    CHKW    18, 18
    movlw   1
    movwf   SCRATCH, a
    dcfsnz  SCRATCH, f, a
    movlw   19
    CHKW    19, 19
; INCFSZ: 0xFF -> 0 skips; with d = 0 the register is left alone
    setf    SCRATCH, a
    movlw   20
    incfsz  SCRATCH, f, a
    movlw   0xEE
    CHKW    20, 20
    incfsz  SCRATCH, w, a
    movlw   21
    CHKW    21, 21
    CHKF    SCRATCH, 0, 22
; INFSNZ
    movlw   23
    infsnz  SCRATCH, f, a
    movlw   0xEE
    CHKW    23, 23
    setf    SCRATCH, a
    infsnz  SCRATCH, f, a
    movlw   24
    CHKW    24, 24
; TSTFSZ
    movlw   25
    tstfsz  SCRATCH, a
    movlw   0xEE
    CHKW    25, 25
    movlw   1
    movwf   SCRATCH, a
    tstfsz  SCRATCH, a
    movlw   26
    CHKW    26, 26
; none of the skips touch STATUS
    movlw   0x1F
    movwf   STATUS, a
    movlw   5
    movwf   SCRATCH, a
    decfsz  SCRATCH, f, a
    nop
    incfsz  SCRATCH, f, a
    nop
    cpfseq  SCRATCH, a
    nop
    tstfsz  SCRATCH, a
    nop
    CHKST   0x1F, 27
; a skip at the end of a BRA target: skip over a BRA
    movlw   28
    btfsc   SCRATCH, 7, a
    bra     _fail
    movlw   29
    btfss   SCRATCH, 7, a
    bra     ok29
    goto    _fail
ok29:
    TESTEND
    END
