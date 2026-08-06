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
;   6. an SPI read from each of two expanders, on different chip selects,
;      whose pins the model drives
;   7. a WS2812 frame bit-banged on RB7, which QEMU decodes and reports to the
;      model as colours rather than as edges
;
; Points 5 and 6 are the interesting ones: they come back by different routes
; -- one through a SoC pin, one through a chip on the SPI bus -- and both
; originate outside QEMU. Two expanders rather than one because a board is
; described on the command line now, and nothing else would notice if the
; second chip were quietly wired to the first one's select.
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
LEDB    equ 0x76
LEDN    equ 0x77

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

;-------------------------------------------------------------- SendByte
; Shifts W out of RB7 in WS2812 form, most significant bit first. Bank 0.
;
; A WS2812 bit is a high pulse whose share of the bit period carries the
; value: about a third for a zero, two thirds for a one. QEMU decodes it by
; comparing each high against half of the period it measures from the frame's
; own first bit, so what matters here is the ratio and not the nanoseconds --
; which is just as well, because how much virtual time an instruction takes is
; the -icount shift's business and not this program's.
;
; Both paths through the bit are padded to the same length so that every bit
; has the same period: nine slots high out of fourteen is a one, three out of
; fourteen is a zero, and the halfway mark has two slots of clearance either
; side. That margin is deliberate -- the emulator charges one cycle for every
; instruction where hardware charges two for a taken branch, and the decode
; still lands correctly if that ever changes.
SendByte:
    movwf   LEDB
    movlw   8
    movwf   LEDN
_led_bit:
    bsf     0x19, 7             ; LATB7 high, and the bit starts
    btfss   LEDB, 7
    goto    _led_zero
    nop                         ; a one holds it high
    nop
    nop
    nop
    nop
    nop
    nop
    bcf     0x19, 7
    goto    _led_next
_led_zero:
    bcf     0x19, 7             ; a zero has let go already
    nop
    nop
    nop
    nop
    nop
    nop
    nop
_led_next:
    rlf     LEDB, 1
    decfsz  LEDN, 1
    goto    _led_bit
    return

;-------------------------------------------------------------- ReadGpio
; Reads the selected expander's GPIO register into HEXV. Whichever chip has
; its select asserted is the one that answers.
ReadGpio:
    movlw   0x41
    call    SpiXfer
    movlw   0x09
    call    SpiXfer
    movlw   0x00
    call    SpiXfer
    movwf   HEXV
    return

;--------------------------------------------------------------- Pixels
; One pixel each, in the WS2812's green-red-blue order.
Pixel_Red:
    movlw   0x00
    call    SendByte
    movlw   0xFF
    call    SendByte
    movlw   0x00
    call    SendByte
    return

Pixel_Blue:
    movlw   0x00
    call    SendByte
    movlw   0x00
    call    SendByte
    movlw   0xFF
    call    SendByte
    return

Pixel_Green:
    movlw   0xFF
    call    SendByte
    movlw   0x00
    call    SendByte
    movlw   0x00
    call    SendByte
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
    movlw   0x88
    movwf   0x1A                ; LATC, both chip selects idle high
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
; Two of them, on RC7 and RC3. Both answer the same opcode -- 0x41 is a read,
; register 0x09 is GPIO -- and only the one whose select is low replies, which
; is the whole point of asking twice.
    EMIT 'G'
    EMIT 'P'
    EMIT '='
    SETBANK 0
    bcf     0x1A, 7             ; RC7 low
    call    ReadGpio
    SETBANK 0
    bsf     0x1A, 7
    movf    HEXV, 0
    call    PutHex
    EMIT 13
    EMIT 10

    EMIT 'G'
    EMIT 'Q'
    EMIT '='
    SETBANK 0
    bcf     0x1A, 3             ; RC3 low
    call    ReadGpio
    SETBANK 0
    bsf     0x1A, 3
    movf    HEXV, 0
    call    PutHex
    EMIT 13
    EMIT 10

;--------------------------------------------------- 7. a WS2812 frame
; Four pixels: two red, one blue, one green. The wire order is GRB, and QEMU
; turns the whole frame back into colours for the model, so what the test
; checks is a summary and not a waveform.
    SETBANK 0
    call    Pixel_Red
    call    Pixel_Red
    call    Pixel_Blue
    call    Pixel_Green

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
