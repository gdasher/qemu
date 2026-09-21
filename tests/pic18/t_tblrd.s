; Table reads: every TBLPTR mode from a flash table, the carry into
; TBLPTRH, the configuration bytes at 0x300000, and the reset vector.
;
; SPDX-License-Identifier: GPL-2.0-or-later

    include "pic18test.inc"
    CONFIG MVECEN = OFF
    CONFIG STVREN = OFF
    CONFIG WDTE = OFF

    PSECT resetvec,abs,class=CODE,reloc=2
    ORG 0
    goto    main

    PSECT text,abs,class=CODE,reloc=2
    ORG 0x100
main:
    movlw   low table
    movwf   TBLPTRL, a
    movlw   high table
    movwf   TBLPTRH, a
    movlw   low highword table
    movwf   TBLPTRU, a
    tblrd   *
    movf    TABLAT, w, a
    CHKW    0x11, 1
    movf    TBLPTRL, w, a
    CHKW    low table, 2
    tblrd   *+
    movf    TABLAT, w, a
    CHKW    0x11, 3
    movf    TBLPTRL, w, a
    CHKW    low table + 1, 4
    tblrd   *+
    movf    TABLAT, w, a
    CHKW    0x22, 5
    tblrd   +*
    movf    TABLAT, w, a
    CHKW    0x44, 6
    tblrd   *-
    movf    TABLAT, w, a
    CHKW    0x44, 7
    movf    TBLPTRL, w, a
    CHKW    low table + 2, 8
    tblrd   *
    movf    TABLAT, w, a
    CHKW    0x33, 9
; a word is stored low byte first
    movlw   low table + 4
    movwf   TBLPTRL, a
    tblrd   *+
    movf    TABLAT, w, a
    CHKW    0xEF, 10
    tblrd   *
    movf    TABLAT, w, a
    CHKW    0xBE, 11
; the increment carries into TBLPTRH
    movlw   0xFF
    movwf   TBLPTRL, a
    clrf    TBLPTRH, a
    tblrd   *+
    movf    TBLPTRH, w, a
    CHKW    1, 12
    movf    TBLPTRL, w, a
    CHKW    0, 13
; configuration bytes: CONFIG3 (MVECEN off), CONFIG4 (STVREN off),
; CONFIG5 (WDTE off), as pic-as writes them for the directives above
    movlw   0x30
    movwf   TBLPTRU, a
    clrf    TBLPTRH, a
    movlw   2
    movwf   TBLPTRL, a
    tblrd   *+
    movf    TABLAT, w, a
    CHKW    0xF7, 14
    tblrd   *+
    movf    TABLAT, w, a
    CHKW    0xEF, 15
    tblrd   *
    movf    TABLAT, w, a
    CHKW    0x9F, 16
; TBLPTRU keeps six bits
    movlw   0xFF
    movwf   TBLPTRU, a
    movf    TBLPTRU, w, a
    CHKW    0x3F, 17
; the reset vector: GOTO main is EF80 F000, low byte first
    clrf    TBLPTRU, a
    clrf    TBLPTRH, a
    clrf    TBLPTRL, a
    tblrd   *+
    movf    TABLAT, w, a
    CHKW    0x80, 18
    tblrd   *
    movf    TABLAT, w, a
    CHKW    0xEF, 19
; TABLAT is writable
    movlw   0x5A
    movwf   TABLAT, a
    movf    TABLAT, w, a
    CHKW    0x5A, 20
    TESTEND
table:
    db      0x11, 0x22, 0x33, 0x44
    dw      0xBEEF
    END
