;===========================================================================
; PFLASH.COM - PICO9918 Flash Utility v0.2
; Created by GTAMP (c) 2026
;
; This is AI generated code. No warranty. Use at own risk.
;
; DESCRIPTION:
;   A CP/M utility to flash UF2 firmware files to the PICO9918 VDP.
;
; ASSEMBLY:
;   sjasmplus pflash.asm --raw=pflash.com
;
; USAGE:
;   A>PFLASH <filename.uf2> [/S]
;   /S : Verify first 10 blocks only (skip full pass).
;
; Don't contact visrealm for support with this third party tool
;
; You can reflash using the USB cable if something goes wrong
;
; https://gtamp.com/nabu
; https://github.com/visrealm/pico9918/

; --- CP/M System Constants ---
WBOOT       EQU     0x0000      ; Warm boot entry
BDOS        EQU     0x0005      ; BDOS function caller
FCB1        EQU     0x005C      ; Default File Control Block
TAIL        EQU     0x0080      ; Command line parameter buffer
F_OPENF     EQU     15          ; Open file
F_READSEQ   EQU     20          ; Read sequential record (128 bytes)
F_SETDMA    EQU     26          ; Set DMA address
F_CONIN     EQU     1           ; Console input (wait for key)
F_PRTSTR    EQU     9           ; Print string (ending in $)
F_READBUF   EQU     10          ; Read buffered line
CR          EQU     0x0D
LF          EQU     0x0A

; --- PICO9918 SR#2 flash status bits ---
SR2_RUNNING EQU     0x80        ; bit 7: 1 = operation in progress
SR2_ERR_MSK EQU     0x1C        ; bits[4:2]: error code (0 = OK)

; --- Hardware Constants ---
VRAM_ADDR   EQU     0x1D00      ; Target VRAM staging area
FLASH_WRCMD EQU     0xC0 | (VRAM_ADDR >> 8)  ; Trigger reg 63

; --- VDP border colours ---
COL_GREEN   EQU     0x0C
COL_RED     EQU     0x06

            ORG     0x0100

;===========================================================================
; Main Entry Point
;===========================================================================
main:
    LD      SP, stack_top

    LD      A, (FCB1+1)
    CP      ' '
    JP      Z, show_usage

    LD      DE, s_banner : CALL prt

    ; --- 1. Parse Command Line Arguments ---
    XOR     A
    LD      (v_skip_flag), A
    LD      HL, TAIL
    LD      B, (HL)
    INC     HL
cli_loop:
    LD      A, B : OR A : JR Z, cli_done
    LD      A, (HL) : CP '/' : JR NZ, .next_char
    INC     HL : DEC B : JR Z, cli_done
    LD      A, (HL) : AND 0x5F
    CP      'S' : JR NZ, .next_char
    LD      A, 1 : LD (v_skip_flag), A
.next_char:
    INC     HL : DEC B : JR cli_loop
cli_done:

    ; --- 2. Interactive Port Configuration ---
    LD      DE, s_prompt_port : CALL prt
    LD      DE, input_buf : LD C, F_READBUF : CALL BDOS
    LD      A, (input_buf+1) : OR A : JR Z, use_default

    LD      HL, input_buf+2
    CALL    asc2hex : RLC A : RLC A : RLC A : RLC A
    LD      B, A
    INC     HL : CALL asc2hex : OR B
    LD      (v_vdp_d), A
    JR      calc_port
use_default:
    LD      A, 0xA0 : LD (v_vdp_d), A
calc_port:
    INC     A : LD (v_vdp_c), A
    LD      DE, s_using_port : CALL prt
    LD      A, (v_vdp_d) : CALL prt_hex_byte : CALL crlf

    ; --- 3. File Initialization ---
    CALL    fcb_rewind
    LD      DE, FCB1 : LD C, F_OPENF : CALL BDOS
    INC     A : JP Z, err_nofile

    ; --- 4. Verification Pass ---
    LD      DE, s_v_lbl : CALL prt
    LD      HL, 0 : LD (v_cur), HL : LD (v_total), HL
    XOR     A : LD (v_first_block), A

v_vloop:
    CALL    read_block : JR C, v_vdone
    CALL    check_magic : JP NZ, err_magic
    LD      A, (v_first_block) : OR A : JR NZ, .not_first
    LD      HL, (uf2_buf + 24) : LD (v_total), HL
    LD      A, 1 : LD (v_first_block), A
.not_first:
    LD      HL, (v_cur) : INC HL : LD (v_cur), HL
    CALL    show_counter
    LD      HL, (v_total) : LD DE, (v_cur)
    OR      A : SBC HL, DE : JR Z, v_vdone
    LD      A, (v_skip_flag) : OR A : JR Z, v_vloop
    ; /S mode: continue until 10 blocks have been checked
    LD      HL, (v_cur) : LD DE, 10
    OR      A : SBC HL, DE : JR NC, v_vdone   ; v_cur >= 10 → stop
    JR      v_vloop
v_vdone:
    LD      HL, (v_cur) : LD A, H : OR L : JP Z, err_empty
    CALL    crlf : LD DE, s_valid : CALL prt
    LD      DE, s_warn : CALL prt

v_ask:
    LD      C, F_CONIN : CALL BDOS : AND 0x5F
    CP      'Y' : JR Z, v_start
    CP      'N' : JP Z, WBOOT
    JR      v_ask

;===========================================================================
; Flash Engine
;===========================================================================
v_start:
    CALL    crlf
    CALL    fcb_rewind
    LD      DE, FCB1 : LD C, F_OPENF : CALL BDOS

    LD      A, 0x82 : LD B, 1  : CALL write_vreg  ; Disable display
    LD      A, 0x1C : LD B, 57 : CALL write_vreg  ; Unlock PICO9918
    LD      A, 0x1C : LD B, 57 : CALL write_vreg  ; Double-clutch
    LD      A, 2   : LD B, 15 : CALL write_vreg   ; SR pointer → 2

    LD      DE, s_f_lbl : CALL prt
    LD      HL, 0 : LD (v_cur), HL

flash_loop:
    CALL    read_block : JP C, flash_done

    DI

    ; 1. Set VRAM write address to 0x1D00
    LD      A, (v_vdp_c) : LD C, A
    IN      A, (C)                              ; clear any pending status
    LD      A, VRAM_ADDR & 0xFF : OUT (C), A
    LD      A, 0x40 | (VRAM_ADDR >> 8) : OUT (C), A

    ; 2. Stream 292-byte shortened block into VRAM:
    ;    header (32) + payload (256) + end magic (4)
    LD      A, (v_vdp_d) : LD C, A
    LD      HL, uf2_buf
    LD      B, 32                               ; header
.v_vh:
    LD      A, (HL) : OUT (C), A : INC HL : DJNZ .v_vh
    LD      B, 0                                ; payload (256 iterations)
.v_vp:
    LD      A, (HL) : OUT (C), A : INC HL : DJNZ .v_vp
    LD      HL, uf2_buf + 508
    LD      B, 4                                ; end magic
.v_ve:
    LD      A, (HL) : OUT (C), A : INC HL : DJNZ .v_ve

    ; 3. Trigger flash write: reg 63 = FLASH_WRCMD
    LD      A, FLASH_WRCMD : LD B, 63 : CALL write_vreg

    ; 4. Poll SR#2 – two phases:
    ;    Phase A: spin until bit7 = 1  (RP2040 has started the operation)
    ;    Phase B: spin until bit7 = 0  (operation complete)
    ;
    ;    Without phase A there is a race: if we read SR#2 before the RP2040
    ;    has asserted bit7 we see 0x00 (idle/no error) and incorrectly
    ;    declare success, leaving the block un-programmed.
    ;
    ;    C is loaded once here; reloading (v_vdp_c) inside the loop wastes
    ;    ~10 T-states per iteration for a value that never changes.
    LD      A, (v_vdp_c) : LD C, A

.poll_a:
    IN      A, (C)
    BIT     7, A
    JR      Z, .poll_a              ; wait for busy = 1

.poll_b:
    IN      A, (C)
    BIT     7, A
    JR      NZ, .poll_b             ; wait for busy = 0

    ; 5. Check SR#2 error field – bits[4:2]
    ;    0 = OK; 1=bad header 2=sequence 3=size 4=verify 5=busy
    AND     SR2_ERR_MSK
    JR      NZ, flash_error         ; non-zero → something went wrong

    EI

    LD      HL, (v_cur) : INC HL : LD (v_cur), HL
    CALL    show_counter

    LD      HL, (v_total) : LD DE, (v_cur)
    OR      A : SBC HL, DE : JR Z, flash_done
    JP      flash_loop

;---------------------------------------------------------------------------
; Flash error handler
;   A holds the raw SR#2 error field (bits[4:2] set, others masked).
;   Border goes RED, error code is decoded and printed, then we exit.
;---------------------------------------------------------------------------
flash_error:
    EI
    ; Border → RED
    LD      A, COL_RED : LD B, 7 : CALL write_vreg

    RRCA : RRCA                     ; shift bits[4:2] → bits[2:0], code 1-5
    PUSH    AF
    LD      DE, s_ferr_pfx : CALL prt
    POP     AF
    CALL    err_lookup              ; DE → error string for this code
    CALL    prt
    CALL    crlf
    LD      DE, s_ferr_sfx : CALL prt
    LD      C, F_CONIN : CALL BDOS

    ; Re-enable display and warm boot
    LD      A, 0xC2 : LD B, 1 : CALL write_vreg
    LD      A, 0x01 : LD B, 7 : CALL write_vreg
    JP      WBOOT

flash_done:
    ; Border → GREEN
    LD      A, COL_GREEN : LD B, 7 : CALL write_vreg
    CALL    crlf : LD DE, s_done : CALL prt
    LD      C, F_CONIN : CALL BDOS

    ; Re-enable display and warm boot
    LD      A, 0xC2 : LD B, 1 : CALL write_vreg
    LD      A, 0x01 : LD B, 7 : CALL write_vreg
    JP      WBOOT

;===========================================================================
; Subroutines
;===========================================================================

show_usage:
    LD      DE, s_banner : CALL prt
    LD      DE, s_usage  : CALL prt
    LD      DE, s_skip_help : CALL prt
    JP      WBOOT

; Convert ASCII character at (HL) to hex nibble in A
asc2hex:
    LD      A, (HL) : AND 0x5F
    CP      'A' : JR NC, .is_alpha
    SUB     '0' : AND 0x0F : RET
.is_alpha:
    SUB     'A' - 10 : AND 0x0F : RET

; Reset FCB pointers for re-reading
fcb_rewind:
    XOR     A
    LD      (FCB1+12), A : LD (FCB1+32), A
    RET

; Read 512 bytes (4 CP/M sectors) into uf2_buf
read_block:
    LD      HL, uf2_buf : LD B, 4
.r_loop:
    PUSH    BC : PUSH HL
    EX      DE, HL : LD C, F_SETDMA : CALL BDOS
    LD      DE, FCB1 : LD C, F_READSEQ : CALL BDOS
    POP     HL : POP BC
    OR      A : JR NZ, .r_err
    LD      DE, 128 : ADD HL, DE
    DJNZ    .r_loop
    OR      A : RET
.r_err:
    SCF : RET

; Output hex byte in A to console
prt_hex_byte:
    PUSH    AF : RRA : RRA : RRA : RRA : CALL .ph1 : POP AF
.ph1:
    AND     0x0F : CP 10 : JR C, .ph2 : ADD A, 7
.ph2:
    ADD     A, '0' : LD E, A : LD C, 2 : JP BDOS

; Write VDP register B with value A
write_vreg:
    PUSH    AF : LD A, (v_vdp_c) : LD C, A : POP AF
    OUT     (C), A : LD A, 0x80 : OR B : OUT (C), A : RET

; Display x/y counter on same line
show_counter:
    PUSH    HL
    LD      E, CR : LD C, 2 : CALL BDOS
    LD      HL, (v_cur)   : CALL print_u16
    LD      DE, s_slash   : CALL prt
    LD      HL, (v_total) : CALL print_u16
    LD      DE, s_clear   : CALL prt
    POP     HL : RET

; Verify UF2 magic numbers
check_magic:
    LD      HL, uf2_buf       : LD DE, d_m0 : LD B, 4 : CALL .v_cb : RET NZ
    LD      HL, uf2_buf + 4   : LD DE, d_m1 : LD B, 4 : CALL .v_cb : RET NZ
    LD      HL, uf2_buf + 508 : LD DE, d_me : LD B, 4
.v_cb:
    LD      A, (DE) : CP (HL) : RET NZ : INC HL : INC DE : DJNZ .v_cb : RET

; Print 16-bit unsigned integer in HL
print_u16:
    LD      B, 0
    LD      DE, 10000 : CALL .pu1
    LD      DE, 1000  : CALL .pu1
    LD      DE, 100   : CALL .pu1
    LD      DE, 10    : CALL .pu1
    LD      A, L : ADD A, '0' : LD E, A : LD C, 2 : JP BDOS
.pu1:
    LD      A, '0' - 1
.psb:
    INC     A : OR A : SBC HL, DE : JR NC, .psb
    ADD     HL, DE : CP '0' : JR NZ, .pem
    LD      A, B : OR A : RET Z
    LD      A, '0'
.pem:
    LD      B, 1 : PUSH HL : PUSH BC
    LD      E, A : LD C, 2 : CALL BDOS
    POP     BC : POP HL : RET

; err_lookup – A = flash error code (1-5) → DE = string pointer
err_lookup:
    CP      6
    JR      NC, .unk
    DEC     A : ADD A, A            ; × 2 for 16-bit table
    LD      E, A : LD D, 0
    LD      HL, err_tab : ADD HL, DE
    LD      E, (HL) : INC HL : LD D, (HL)
    RET
.unk:
    LD      DE, s_err_unk : RET

prt:  LD C, F_PRTSTR : JP BDOS
crlf: LD DE, s_crlf : CALL prt : RET

; --- Error Handlers ---
err_nofile: LD DE, s_nofile   : CALL prt : JP WBOOT
err_magic:  LD DE, s_errmagic : CALL prt : JP WBOOT
err_empty:  LD DE, s_errempty : CALL prt : JP WBOOT

;===========================================================================
; Data & Strings
;===========================================================================
s_banner:
    DB      CR, LF, "PICO9918 Flash v0.2 by GTAMP", CR, LF, "$"
s_usage:
    DB      "Usage: PFLASH <file.uf2> [/S]", CR, LF, "$"
s_skip_help:
    DB      "Use /S to skip full verify", CR, LF, "$"
s_prompt_port:
    DB      "VDP port (enter for A0): $"
s_using_port:
    DB      CR, LF, "VDP Port: $"
s_v_lbl:
    DB      CR, LF, "Verifying file: ", CR, LF, "$"
s_f_lbl:
    DB      CR, LF, "Flashing: ", CR, LF, "$"
s_valid:
    DB      CR, LF, "File OK.$"
s_warn:
    DB      CR, LF, "Screen will be BLANK during flash"
    DB      CR, LF, "Screen will turn GREEN when done"
    DB      CR, LF, "Screen will turn RED if an error occurs"
    DB      CR, LF, "Update installs after power cycle"
    DB      CR, LF, "Proceed? (y/n): $"
s_done:
    DB      CR, LF, "SUCCESS! Press key to exit.$"
s_ferr_pfx:
    DB      CR, LF, "FLASH ERROR: $"
s_ferr_sfx:
    DB      CR, LF, "Press any key to exit.$"
s_slash:
    DB      "/$"
s_clear:
    DB      "      $"
s_crlf:
    DB      CR, LF, "$"
s_errmagic:
    DB      CR, LF, "Error: Invalid UF2 magic.$"
s_nofile:
    DB      CR, LF, "Error: File not found.$"
s_errempty:
    DB      CR, LF, "Error: File is empty.$"

; Flash error strings (codes 1-5)
s_err1:     DB  "Bad UF2 header$"
s_err2:     DB  "Block sequence error$"
s_err3:     DB  "Wrong payload size$"
s_err4:     DB  "Verify failed$"
s_err5:     DB  "Device busy$"
s_err_unk:  DB  "Unknown error$"

err_tab:    DW  s_err1, s_err2, s_err3, s_err4, s_err5

; UF2 Magic Numbers
d_m0:       DB  0x55, 0x46, 0x32, 0x0A
d_m1:       DB  0x57, 0x51, 0x5D, 0x9E
d_me:       DB  0x30, 0x6F, 0xB1, 0x0A

; Variables
v_vdp_d:        DB  0xA0
v_vdp_c:        DB  0xA1
v_cur:          DW  0
v_total:        DW  0
v_first_block:  DB  0
v_skip_flag:    DB  0

            DS  64, 0
stack_top:

            ALIGN 256
input_buf:  DB  4, 0, 0, 0, 0, 0
uf2_buf:    DS  512, 0
