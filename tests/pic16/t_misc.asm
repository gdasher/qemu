; Instructions that no XC8 build of the reference firmware emits, so they get
; no coverage from validating the decoder against a real image: CLRW, CLRWDT,
; TRIS, BRW and CALLW. RESET and SLEEP are covered by their own fixtures.
;
; Also covers MOVLB in this family's 6-bit encoding, which gputils cannot
; assemble, so it is emitted with dw.
;
; SPDX-License-Identifier: GPL-2.0-or-later

    processor 16f1829
    radix dec
    include "pictest.inc"

    org 0
    goto    main

    org 8
main:
;-------------------------------------------------------------------- CLRW
    movlw   0xFF
    clrw
    CHKW    0x00, 1
    CHKSET  STATUS, Z, 2

;------------------------------------------------------------------ CLRWDT
; Sets both TO and PD.
    clrwdt
    CHKSET  STATUS, TO, 3
    CHKSET  STATUS, PD, 4

;-------------------------------------------------------------------- TRIS
; Unmodelled, but it must decode and fall through rather than trap.
    movlw   0x0F
    tris    5
    tris    6
    tris    7
    CHKW    0x0F, 5

;--------------------------------------------------------------------- BRW
; PC = (address of next instruction) + W, so W=3 lands on the fourth entry.
    movlw   3
    brw
    goto    _brw_bad            ; W = 0
    goto    _brw_bad            ; W = 1
    goto    _brw_bad            ; W = 2
    goto    _brw_ok             ; W = 3
_brw_bad:
    movlw   6
    goto    _fail
_brw_ok:

;------------------------------------------------------------------- CALLW
; PC[7:0] comes from W and PC[14:8] from PCLATH, so the target must be
; reachable with PCLATH left at zero.
    movlw   0x00
    movwf   PCLATH
    movlw   low _callw_target
    callw
    CHKW    0x5A, 7

;------------------------------------------------------------------- MOVLB
; 6-bit literal at 0x0140, which gputils will not assemble for this family.
    dw      0x0140 + 63         ; movlb 63
    movlw   63
    xorwf   BSR, 0              ; W = BSR ^ 63, zero if BSR took the value
    movlw   8
    btfss   STATUS, Z
    goto    _fail

; A bank above 31 is only reachable with the 6-bit form, so this doubles as
; proof that the literal is not being truncated to five bits.
    dw      0x0140 + 5          ; movlb 5
    movlw   5
    xorwf   BSR, 0
    movlw   9
    btfss   STATUS, Z
    goto    _fail

;------------------------------------------------------------------- MOVLP
    movlw   0x7F
    movwf   PCLATH
    CHKF    PCLATH, 0x7F, 10
    clrf    PCLATH

    goto    _pass

_callw_target:
    movlw   0x5A
    return

    TESTEND
    end
