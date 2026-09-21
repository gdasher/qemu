; MOVFF and MOVFFL: between banks, into and out of the access bank, to and
; from WREG, through indirect operands, to the top of the 14-bit space, and
; without touching STATUS.
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
    movlb   5
    movlw   0x11
    movwf   0x80, b
    movff   0x580, 0x680
    movlb   6
    movf    0x80, w, b
    CHKW    0x11, 1
    movff   0x680, SCRATCH_ABS
    CHKF    SCRATCH, 0x11, 2
    movlw   0x22
    movff   WREG, RESULT_ABS
    CHKF    RESULT, 0x22, 3
    movff   SCRATCH_ABS, WREG
    CHKW    0x11, 4
; indirect source and destination
    movlb   5
    movlw   0x33
    movwf   0x81, b
    lfsr    0, 0x580
    lfsr    1, 0x700
    movff   POSTINC0, POSTINC1
    movff   POSTINC0, POSTINC1
    movlb   7
    movf    0x00, w, b
    CHKW    0x11, 5
    movf    0x01, w, b
    CHKW    0x33, 6
    movf    FSR0L, w, a
    CHKW    0x82, 7
    movf    FSR1L, w, a
    CHKW    0x02, 8
; MOVFFL reaches beyond the 12-bit range of MOVFF
    movlw   0x44
    movwf   SAVE, a
    movffl  SAVE_ABS, 0x2000
    movffl  0x2000, COUNT_ABS
    CHKF    COUNT, 0x44, 9
    movlb   0x20
    movf    0x00, w, b
    CHKW    0x44, 10
    movlw   0x55
    movff   WREG, SCRATCH_ABS
    movffl  SCRATCH_ABS, 0x24FF
    movlb   0x24
    movf    0xFF, w, b
    CHKW    0x55, 11
    movffl  0x24FF, 0x3FFF
    movlb   0x3F
    movf    0xFF, w, b
    CHKW    0x55, 12
; into a core register
    movlw   0x66
    movff   WREG, FSR2L
    movf    FSR2L, w, a
    CHKW    0x66, 13
; neither touches STATUS
    clrf    COUNT, a
    movlw   0x1F
    movwf   STATUS, a
    movff   COUNT_ABS, RESULT_ABS
    movffl  COUNT_ABS, RESULT_ABS
    CHKST   0x1F, 14
    TESTEND
    END
