; Control flow: CALL/RCALL/RETURN/RETLW/CALLW, the fast call shadows, GOTO,
; BRA, every conditional branch, PUSH/POP, STKPTR and TOS access, computed
; GOTO through PCL, and the PCLATH latch on a PCL read.
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
; CALL and RETURN
    movlw   1
    call    sub_inc
    CHKW    2, 1
; RCALL
    movlw   10
    rcall   sub_inc
    CHKW    11, 2
; RETLW
    call    sub_retlw
    CHKW    0x42, 3
; CALLW takes PCLATU:PCLATH:W
    movlw   low highword sub_mark
    movwf   PCLATU, a
    movlw   high sub_mark
    movwf   PCLATH, a
    clrf    RESULT, a
    movlw   low sub_mark
    callw
    CHKF    RESULT, 0x77, 4
    clrf    PCLATH, a
    clrf    PCLATU, a
; CALL fast saves W, STATUS and BSR; RETURN fast restores them
    movlw   0x1F
    movwf   STATUS, a
    movlw   0x2A
    movwf   BSR, a
    movlw   0x55
    call    sub_trash, f
    movwf   RESULT, a
    CHKST   0x1F, 5
    CHKF    RESULT, 0x55, 6
    movf    BSR, w, a
    CHKW    0x2A, 7
; a plain RETURN restores nothing, even with the shadows loaded
    movlw   0x2A
    movwf   BSR, a
    call    sub_trash_plain
    movf    BSR, w, a
    CHKW    0, 8
; GOTO and BRA forward
    movlw   9
    goto    g1
    movlw   0xEE
g1:
    CHKW    9, 9
    movlw   10
    bra     b1
    movlw   0xEE
b1:
    CHKW    10, 10
; conditional branches on each flag, taken and not taken
    movlw   0x01
    movwf   STATUS, a
    movlw   11
    bc      c_ok
    goto    _fail
c_ok:
    movlw   12
    bnc     _fail
    movlw   0x1E
    movwf   STATUS, a
    movlw   13
    bnc     nc_ok
    goto    _fail
nc_ok:
    movlw   14
    bc      _fail
    movlw   15
    bz      z_ok
    goto    _fail
z_ok:
    movlw   16
    bnz     _fail
    movlw   17
    bov     ov_ok
    goto    _fail
ov_ok:
    movlw   18
    bnov    _fail
    movlw   19
    bn      n_ok
    goto    _fail
n_ok:
    movlw   20
    bnn     _fail
    movlw   0
    movwf   STATUS, a
    movlw   21
    bnz     nz_ok
    goto    _fail
nz_ok:
    movlw   22
    bz      _fail
    movlw   23
    bnov    nov_ok
    goto    _fail
nov_ok:
    movlw   24
    bov     _fail
    movlw   25
    bnn     nn_ok
    goto    _fail
nn_ok:
    movlw   26
    bn      _fail
; a backward BRA loop
    clrf    COUNT, a
back:
    incf    COUNT, f, a
    movlw   3
    cpfseq  COUNT, a
    bra     back
    CHKF    COUNT, 3, 27
; PUSH: TOS holds the address of the next instruction
    push
here:
    movf    TOSL, w, a
    CHKW    low here, 28
    movf    TOSH, w, a
    CHKW    high here, 29
    movf    TOSU, w, a
    CHKW    0, 30
    movf    STKPTR, w, a
    CHKW    1, 31
    pop
    movf    STKPTR, w, a
    CHKW    0, 32
; writing TOS and returning is a jump
    push
    movlw   low tos_target
    movwf   TOSL, a
    movlw   high tos_target
    movwf   TOSH, a
    clrf    TOSU, a
    movlw   33
    return
    goto    _fail
tos_target:
    CHKW    33, 33
; STKPTR is writable
    movlw   5
    movwf   STKPTR, a
    movf    STKPTR, w, a
    CHKW    5, 34
    clrf    STKPTR, a
; computed GOTO: ADDWF PCL reads PC + 2 and latches PCLATH from it
    movlw   4
    call    jump_table
    CHKW    0xB2, 35
; reading PCL latches PCLATH and PCLATU
    clrf    PCLATH, a
    movf    PCL, w, a
pcl_next:
    movf    PCLATH, w, a
    CHKW    high pcl_next, 36
    movf    PCL, w, a
pcl_next2:
    CHKW    low pcl_next2, 37
; a plain PCL write jumps within the latched page
    movlw   high pcl_target
    movwf   PCLATH, a
    clrf    PCLATU, a
    movlw   low pcl_target
    movwf   PCL, a
    movlw   38
    goto    _fail
pcl_target:
    TESTEND

sub_inc:
    addlw   1
    return
sub_retlw:
    retlw   0x42
sub_mark:
    movlw   0x77
    movwf   RESULT, a
    return
sub_trash:
    clrf    STATUS, a
    clrf    BSR, a
    movlw   0
    return  f
sub_trash_plain:
    clrf    STATUS, a
    clrf    BSR, a
    movlw   0
    return

; The table must not cross a 256-byte boundary: ADDWF PCL only reaches
; within the page PCLATH was latched with.
    PSECT jt,abs,class=CODE,reloc=2
    ORG 0x800
jump_table:
    addwf   PCL, f, a
    retlw   0xB0
    retlw   0xB1
    retlw   0xB2
    retlw   0xB3
    END
