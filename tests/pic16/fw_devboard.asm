; Exercises the whole emulated ecosystem on -M pic16-devboard.
;
; Not a product: it walks every path a PIC16 firmware has for reaching the
; outside world, and reports what it saw over the serial console so a test can
; check it.
;
;   1. a software reset, which must power-cycle the board and not just the core
;   2. a watchdog reset, which must do the same from a timer rather than an
;      instruction
;   3. banner over EUSART1, through PPS-routed pins
;   4. three pulses on RA5, which the model on the bridge counts
;   5. a read of RB5, which the model drives
;   6. an SPI read of the expander's GPIO register, whose pins the model drives
;
; Points 5 and 6 are the interesting ones: they come back by different routes
; -- one through a SoC pin, one through a chip on the SPI bus -- and both
; originate outside QEMU.
;
; The whole program runs three times: the first pass configures everything and
; executes RESET, the second stops feeding the watchdog and waits for it, and
; only the third reports. Nothing is printed before then, so every other check
; still sees exactly one run.
;
; SPDX-License-Identifier: GPL-2.0-or-later

    processor 16f1829
    radix dec

; Core registers, reachable from any bank.
STATUS  equ 0x03
BSR     equ 0x08
C       equ 0
Z       equ 2

; Common RAM: the same sixteen bytes in every bank, so no BSR setup needed.
TMP     equ 0x70
HEXV    equ 0x71
COUNT   equ 0x72
; Data memory survives a reset, so a flag here tells the passes apart.
PASS    equ 0x73
WASPPS  equ 0x74
WASPCON equ 0x75

SETBANK macro n
    movlw   n
    movwf   BSR
    endm

EMIT macro ch
    movlw   ch
    call    PutChar
    endm

    org 0
    goto    Main

    org 8
;----------------------------------------------------------------- PutChar
; Blocks until the transmitter is ready, which the emulated EUSART paces at
; the configured baud rate.
PutChar:
    movwf   TMP
_putc_wait:
    SETBANK 1
    btfss   0x10, 6             ; PIR4, TX1IF
    goto    _putc_wait
    SETBANK 14
    movf    TMP, 0
    movwf   0x0D                ; TX1REG
    return

;------------------------------------------------------------------ PutHex
PutHex:
    movwf   HEXV
    swapf   HEXV, 0
    andlw   0x0F
    call    Nibble
    call    PutChar
    movf    HEXV, 0
    andlw   0x0F
    call    Nibble
    call    PutChar
    return

Nibble:
    addlw   -10
    btfsc   STATUS, C           ; W was 10 or more
    goto    _hex_letter
    addlw   10 + '0'
    return
_hex_letter:
    addlw   'A'
    return

;--------------------------------------------------------------- SpiXfer
; One byte out, one byte back. BF rises when the transfer completes.
SpiXfer:
    movwf   TMP
    SETBANK 15
    movf    TMP, 0
    movwf   0x0C                ; SSP1BUF
_spi_wait:
    btfss   0x0F, 0             ; SSP1STAT, BF
    goto    _spi_wait
    movf    0x0C, 0
    return

;-------------------------------------------------------------------- Main
Main:
; RC1PPS as this pass found it, sampled before anything is configured. The
; earlier passes point it at TX1; a power cycle must have put it back to zero
; by the time the last pass gets here. Resetting only the core would leave the
; peripheral holding what the pass before wrote.
    SETBANK 59
    movf    0x1D, 0             ; RC1PPS
    movwf   WASPPS

; PCON0 says which reset this pass came out of, and has to be read before
; anything else disturbs it.
    SETBANK 3
    movf    0x12, 0             ; PCON0
    movwf   WASPCON

; All pins digital.
    SETBANK 61
    clrf    0x0C                ; ANSELA
    clrf    0x16                ; ANSELB
    clrf    0x20                ; ANSELC

; RA4 and RA5 are outputs; RB5 stays an input; RC0 is the console receive pin
; and the rest of port C drives.
    SETBANK 0
    clrf    0x18                ; LATA
    clrf    0x19                ; LATB
    movlw   0x80
    movwf   0x1A                ; LATC, chip select idle high
    movlw   0xCF
    movwf   0x12                ; TRISA
    movlw   0x3F
    movwf   0x13                ; TRISB
    movlw   0x01
    movwf   0x14                ; TRISC

; Peripheral pin select: console on RC0/RC1, SPI on RB6/RB4/RC2.
    SETBANK 60
    clrf    0x0C                ; PPSLOCK
    movlw   0x10
    movwf   0x42                ; RX1PPS   <- RC0
    movlw   0x13
    SETBANK 59
    movwf   0x1D                ; RC1PPS   -> TX1
    movlw   0x0E
    SETBANK 60
    movwf   0x47                ; SSP1CLKPPS <- RB6
    movlw   0x1B
    SETBANK 59
    movwf   0x1A                ; RB6PPS   -> SCK1
    movlw   0x0C
    SETBANK 60
    movwf   0x48                ; SSP1DATPPS <- RB4
    movlw   0x1C
    SETBANK 59
    movwf   0x1E                ; RC2PPS   -> SDO1
    movlw   0x01
    SETBANK 60
    movwf   0x0C                ; PPSLOCK

; 115200 baud at 32 MHz with BRG16 and BRGH.
    SETBANK 14
    movlw   68
    movwf   0x0E                ; SP1BRGL
    clrf    0x0F                ; SP1BRGH
    bsf     0x12, 3             ; BAUD1CON, BRG16
    bsf     0x11, 2             ; TX1STA, BRGH
    bsf     0x11, 5             ; TX1STA, TXEN
    bsf     0x10, 7             ; RC1STA, SPEN

; SPI host, mode 0, Fosc/64.
    SETBANK 15
    movlw   0x40
    movwf   0x0F                ; SSP1STAT, CKE
    movlw   0x22
    movwf   0x10                ; SSP1CON1, SSPEN and Fosc/64

;----------------------------------------------------------- 1. power cycle
; Everything above is configured now, so each pass has something for its reset
; to undo. Neither of the first two prints anything, which keeps every other
; count at one.
    movf    PASS, 1             ; d=1 writes back; the point is Z
    btfss   STATUS, Z
    goto    _not_first
    movlw   1
    movwf   PASS
    reset

_not_first:
    movlw   2
    subwf   PASS, 0
    btfsc   STATUS, Z
    goto    _third_pass

;-------------------------------------------------------- 2. watchdog reset
; The watchdog is armed out of reset and fires about 2.1 s of virtual time
; after the last CLRWDT. Starving it here has to take the same whole-board
; path the RESET instruction takes -- it expires in a timer callback rather
; than in translated code, and for a long time the two disagreed.
    movlw   2
    movwf   PASS
_starve:
    goto    _starve

_third_pass:
;--------------------------------------------------------------- 3. banner
    EMIT 'P'
    EMIT 'I'
    EMIT 'C'
    EMIT '1'
    EMIT '6'
    EMIT ' '
    EMIT 'O'
    EMIT 'K'
    EMIT 13
    EMIT 10

; Which reset this pass came out of. RWDT reads 0 when the watchdog caused it,
; so this is the watchdog's power cycle reported from the far side of it.
    EMIT 'W'
    EMIT 'D'
    EMIT 'T'
    EMIT '='
    movlw   '0'
    btfsc   WASPCON, 4          ; PCON0, RWDT
    movlw   '1'
    call    PutChar
    EMIT 13
    EMIT 10

; What the power cycle left behind: RC1PPS as this pass first saw it.
    EMIT 'P'
    EMIT 'P'
    EMIT 'S'
    EMIT '='
    movf    WASPPS, 0
    call    PutHex
    EMIT 13
    EMIT 10

;-------------------------------------------------------- 4. pulses on RA5
    movlw   3
    movwf   COUNT
_pulse:
    SETBANK 0
    bsf     0x18, 5             ; LATA5 high
    nop
    bcf     0x18, 5             ; and low again
    decfsz  COUNT, 1
    goto    _pulse

;----------------------------------------------------------- 5. read RB5
    EMIT 'B'
    EMIT '5'
    EMIT '='
    SETBANK 0
    movlw   '0'
    btfsc   0x0D, 5             ; PORTB, RB5
    movlw   '1'
    call    PutChar
    EMIT 13
    EMIT 10

;----------------------------------------------- 6. expander GPIO over SPI
; Opcode 0x41 is a read of device 0; register 0x09 is GPIO.
    EMIT 'G'
    EMIT 'P'
    EMIT '='
    SETBANK 0
    bcf     0x1A, 7             ; chip select low
    movlw   0x41
    call    SpiXfer
    movlw   0x09
    call    SpiXfer
    movlw   0x00
    call    SpiXfer
    movwf   HEXV
    SETBANK 0
    bsf     0x1A, 7             ; chip select high
    movf    HEXV, 0
    call    PutHex
    EMIT 13
    EMIT 10

    EMIT 'D'
    EMIT 'O'
    EMIT 'N'
    EMIT 'E'
    EMIT 13
    EMIT 10

; Feed the watchdog from here on: the run is over, and a second expiry would
; start the whole program again.
_done:
    clrwdt
    goto    _done

    end
