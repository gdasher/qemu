; Exercises the whole emulated ecosystem on -M pic16-devboard.
;
; Not a product: it walks every path a PIC16 firmware has for reaching the
; outside world, and reports what it saw over the serial console so a test can
; check it.
;
;   1. banner over EUSART1, through PPS-routed pins
;   2. three pulses on RA5, which the model on the bridge counts
;   3. a read of RB5, which the model drives
;   4. an SPI read of the expander's GPIO register, whose pins the model drives
;
; The last two are the interesting ones: they come back by different routes --
; one through a SoC pin, one through a chip on the SPI bus -- and both
; originate outside QEMU.
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

;--------------------------------------------------------------- 1. banner
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

;-------------------------------------------------------- 2. pulses on RA5
    movlw   3
    movwf   COUNT
_pulse:
    SETBANK 0
    bsf     0x18, 5             ; LATA5 high
    nop
    bcf     0x18, 5             ; and low again
    decfsz  COUNT, 1
    goto    _pulse

;----------------------------------------------------------- 3. read RB5
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

;----------------------------------------------- 4. expander GPIO over SPI
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

_done:
    goto    _done

    end
