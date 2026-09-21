; SLEEP and wake-up: PD and TO, waking on a pending request with GIE clear
; (execution continues after SLEEP), CLRWDT, and waking into the handler
; with GIE set.
;
; SPDX-License-Identifier: GPL-2.0-or-later

    include "pic18test.inc"
    CONFIG MVECEN = OFF

ISRFLAG EQU 0x10

    PSECT resetvec,abs,class=CODE,reloc=2
    ORG 0
    goto    main

    PSECT hivec,abs,class=CODE,reloc=2
    ORG 8
    goto    isr

    PSECT text,abs,class=CODE,reloc=2
    ORG 0x100
main:
    clrf    ISRFLAG, a
; PD and TO read 1 after power-on
    CHKSET  STATUS, PD_BIT, 1
    CHKSET  STATUS, TO_BIT, 2
; a pending request wakes the core even with GIE clear; SLEEP clears PD
    bcf     INTCON0, GIE, a
    movlw   1
    movff   WREG, IRQH
    bra     s1
s1:
    sleep
    CHKCLR  STATUS, PD_BIT, 3
    CHKSET  STATUS, TO_BIT, 4
    CHKF    ISRFLAG, 0, 5
; CLRWDT sets PD again
    clrwdt
    CHKSET  STATUS, PD_BIT, 6
; with GIE set the still-pending request is taken
    bsf     INTCON0, GIE, a
    nop
    CHKF    ISRFLAG, 1, 7
; A request raised by the instruction before SLEEP is taken before SLEEP,
; as on hardware, so "wake straight into the handler" needs a peripheral
; timer and is left to the SoC tests.
    TESTEND

isr:
    incf    ISRFLAG, f, a
    movlw   0
    movff   WREG, IRQH
    retfie
    END
