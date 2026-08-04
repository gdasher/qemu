; The SLEEP instruction.
;
; expect: halt
;
; SLEEP stops the core until an interrupt arrives, and nothing here can raise
; one, so the guest is expected to stay halted rather than exit. The runner
; treats the resulting timeout as the pass condition; an exit would mean SLEEP
; fell through.
;
; The flag effects (TO set, PD cleared) cannot be observed from the guest
; after it stops, so they are checked before SLEEP is reached: CLRWDT sets
; both, which is the state SLEEP starts from.
;
; SPDX-License-Identifier: GPL-2.0-or-later

    processor 16f1829
    radix dec
    include "pictest.inc"

    org 0
    goto    main

    org 8
main:
    clrwdt
    CHKSET  STATUS, TO, 1
    CHKSET  STATUS, PD, 2

    sleep

; Reaching this means SLEEP did not stop the core.
    movlw   3
    goto    _fail

    TESTEND
    end
