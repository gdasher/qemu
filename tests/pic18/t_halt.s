; SLEEP with nothing pending never returns: the machine must sit idle.
;
; expect: halt
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
    bsf     INTCON0, GIE, a
    sleep
; waking without a request is the failure
    movlw   1
    movff   WREG, EXITC
spin:
    bra     spin
    END
