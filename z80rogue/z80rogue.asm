	; z80rogue for MSX/Colecovision/Memotech
	;
	; by Óscar Toledo Gutiérrez
	;
	; (c) Copyright 2019 Óscar Toledo Gutiérrez
	;
	; Creation date: Sep/29/2019.
	;
	; NABU PC port by GTAMP (c) 2026
	;
	; Assemble wtih gasm80 https://github.com/nanochess/gasm80
	;

NABUCPM:	equ 0		; 0=NABU  1=CPM COM

VDPRAM:		equ $A0		; VDP data port
VDPIO:		equ $A1		; VDP control/status port
KBDDAT:		equ $90		; Keyboard data
KBDSTA:		equ $91		; Keyboard status

ROW_WIDTH:	equ 40
BOX_MAX_WIDTH:	equ 11
BOX_MAX_HEIGHT:	equ 6
BOX_WIDTH:	equ 13
BOX_HEIGHT:	equ 8

GR_VERT:	equ 0xba
GR_TOP_RIGHT:	equ 0xbb
GR_BOT_RIGHT:	equ 0xbc
GR_BOT_LEFT:	equ 0xc8
GR_TOP_LEFT:	equ 0xc9
GR_HORIZ:	equ 0xcd
GR_TUNNEL:	equ 0xb1
GR_DOOR:	equ 0xce
GR_FLOOR:	equ 0xfa
GR_HERO:	equ 0x01
GR_LADDER:	equ 0xf0
GR_TRAP:	equ 0x04
GR_FOOD:	equ 0x05
GR_ARMOR:	equ 0x08
GR_YENDOR:	equ 0x0c
GR_GOLD:	equ 0x0f
GR_WEAPON:	equ 0x18

YENDOR_LEVEL:	equ 26

	; Name table base page value
	; $0800 >> 8 = $08, so (page) variable holds $08 for visible, $0C for hidden
VPAGE_VIS:	equ $08		; visible page high byte ($0800)
VPAGE_HID:	equ $0C		; hidden page high byte  ($0C00)

	; ── Entry point ───────────────────────────────────────────────
    if NABUCPM
	org $0100
	
start:
	di
	xor a
	ld (kbd_char),a
    endif
    if 1-NABUCPM
	org $140D
	nop
	nop
	nop
	di
start:
	ld sp,$FFFF
    endif

	; ── Init VDP ───────────────────────────────────────────────────
	; Each register: write value, then write (reg | $80)
	; push bc / pop bc delay between writes (required by TMS9918A)

	ld a,$00		; R0: mode 0 (text)
	out (VDPIO),a
	push bc
	pop bc
	ld a,$80
	out (VDPIO),a
	push bc
	pop bc

	ld a,$F0		; R1: 16K, video ON, INT enable, text mode
	out (VDPIO),a
	push bc
	pop bc
	ld a,$81
	out (VDPIO),a
	push bc
	pop bc

	ld a,$02		; R2: name table at $0800
	out (VDPIO),a
	push bc
	pop bc
	ld a,$82
	out (VDPIO),a
	push bc
	pop bc

	ld a,$FF		; R3: color table (unused in text mode)
	out (VDPIO),a
	push bc
	pop bc
	ld a,$83
	out (VDPIO),a
	push bc
	pop bc

	ld a,$00		; R4: pattern table at $0000
	out (VDPIO),a
	push bc
	pop bc
	ld a,$84
	out (VDPIO),a
	push bc
	pop bc

	ld a,$7F		; R5: sprite table (unused)
	out (VDPIO),a
	push bc
	pop bc
	ld a,$85
	out (VDPIO),a
	push bc
	pop bc

	ld a,$03		; R6: sprite patterns (unused)
	out (VDPIO),a
	push bc
	pop bc
	ld a,$86
	out (VDPIO),a
	push bc
	pop bc

	ld a,$21		; R7: green on black (original z80rogue colours)
	out (VDPIO),a
	push bc
	pop bc
	ld a,$87
	out (VDPIO),a
	push bc
	pop bc

	; ── Upload font to VRAM $0000 ───────────────────────────────────
	; Set write address $0000
	ld a,$00
	out (VDPIO),a
	push bc
	pop bc
	ld a,$40		; $0000 | write flag
	out (VDPIO),a
	push bc
	pop bc
	; Copy 2048 bytes of font data
	ld hl,letters_bitmaps
	ld bc,$0800		; 2048 bytes
.fontloop:
	ld a,(hl)
	out (VDPRAM),a
	push bc
	pop bc
	inc hl
	dec bc
	ld a,b
	or c
	jr nz,.fontloop

	; ── Clear visible name table ($0800) with spaces ────────────────
	ld a,$00		; VRAM address low = $00
	out (VDPIO),a
	push bc
	pop bc
	ld a,$48		; VRAM address high = $08 | $40 (write)
	out (VDPIO),a
	push bc
	pop bc
	ld bc,$0400		; 1024 bytes
	ld a,$20		; space character
.clrvis:
	out (VDPRAM),a
	push bc
	pop bc
	dec bc
	ld a,b
	or c
	jr nz,.clrvis
	ld a,$20

	; ── Clear hidden name table ($0C00) with spaces ──────────────────
	ld a,$00
	out (VDPIO),a
	push bc
	pop bc
	ld a,$4C		; $0C | $40 (write)
	out (VDPIO),a
	push bc
	pop bc
	ld bc,$0400
	ld a,$20
.clrhid:
	out (VDPRAM),a
	push bc
	pop bc
	dec bc
	ld a,b
	or c
	jr nz,.clrhid
	ld a,$20

	; ── Write test string to top of visible page ─────────────────────
	ld a,$00
	out (VDPIO),a
	push bc
	pop bc
	ld a,$48		; $0800 write
	out (VDPIO),a
	push bc
	pop bc
	ld hl,test
	ld b,16
.testloop:
	ld a,(hl)
	out (VDPRAM),a
	push bc
	pop bc
	inc hl
	djnz .testloop

	; Set (page) = VPAGE_VIS for game logic
	ld a,VPAGE_VIS
	ld (page),a

	; ── Clear keyboard buffer ────────────────────────────────────────
	xor a
	ld (kbd_char),a

	; ── Title screen ─────────────────────────────────────────────────
title_screen:
	; Clear visible page
	ld hl,$0800
	ld bc,$0400
	xor a
	call FILVRM

	ld a,30
	ld (debounce),a

	ld hl,title_letters
	ld de,$0800+5*40
	ld bc,18*40
	call LDIRVM

	; Flash title (use VBLANK polling for timing)
	ld b,5
.1:	push bc
	ld bc,$a107		; R7 = $A1: dark green on black
	call WRTVDP
	call wait_vblank
	ld bc,$b107
	call WRTVDP
	call wait_vblank
	ld bc,$f107
	call WRTVDP
	call wait_vblank
	ld bc,$b107
	call WRTVDP
	call wait_vblank
	ld bc,$a107
	call WRTVDP
	call wait_vblank
	ld bc,$5107
	call WRTVDP
	call wait_vblank
	pop bc
	djnz .1

	call wait_any_key		; any key or joystick button starts game

	xor a
	ld (level),a
	ld (armor),a
	ld a,1
	ld (yendor),a
	ld (weapon),a
	ld hl,16
	ld (hp),hl
	ld a,1
	ld (first),a

generate_dungeon:
	ld a,(yendor)
	ld b,a
	ld a,(level)
	add a,b
	ld (level),a
	or a
	jp z,game_won

	call update_level

	ld hl,(lfsr)
	ld a,h
	and $41
	or $1a
	ld h,a
	ld a,l
	and $82
	or $6d
	ld l,a
	ld (conn),hl

	ld a,VPAGE_HID		; draw into hidden page
	ld (page),a

	ld h,a
	ld l,0
	ld bc,$0400
	xor a
	call FILVRM

	ld hl,ROW_WIDTH*(BOX_HEIGHT/2-2)+(BOX_WIDTH/2-2)
	ld a,(page)
	add a,h
	ld h,a
.7:
	push hl
	push hl
	ld de,ROW_WIDTH+2
	add hl,de
	ld de,(conn)
	srl d
	rr e
	ld (conn),de
	jr nc,.3
	push hl
	ld b,BOX_WIDTH
	ld a,GR_TUNNEL
	call WRTVRM
	inc hl
	djnz $-4
	pop hl
.3:	ld de,(conn)
	srl d
	rr e
	ld (conn),de
	jr nc,.5
	ld b,BOX_HEIGHT
	ld a,GR_TUNNEL
	ld de,ROW_WIDTH
.4:	call WRTVRM
	add hl,de
	djnz .4
.5:
	call random
	ld a,l
.8:	sub BOX_MAX_WIDTH-2
	jr nc,.8
	add a,BOX_MAX_WIDTH-1
	ld (box_w),a
	ld a,h
.9:	sub BOX_MAX_HEIGHT-2
	jr nc,.9
	add a,BOX_MAX_HEIGHT-1
	ld (box_h),a
	srl a
	ld l,a
	ld h,0
	add hl,hl
	add hl,hl
	add hl,hl
	ld d,h
	ld e,l
	add hl,hl
	add hl,hl
	add hl,de
	ld a,(box_w)
	srl a
	ld e,a
	ld d,0
	add hl,de
	ex de,hl
	pop hl
	or a
	sbc hl,de
	ld ix,GR_TOP_LEFT
	ld iy,GR_TOP_RIGHT*256+GR_HORIZ
	call fill
.10:	ld ix,GR_VERT
	ld iy,GR_VERT*256+GR_FLOOR
	call fill
	ld a,(box_h)
	dec a
	ld (box_h),a
	jp p,.10
	ld ix,GR_BOT_LEFT
	ld iy,GR_BOT_RIGHT*256+GR_HORIZ
	call fill
	pop hl
	ld de,BOX_WIDTH
	add hl,de
	ld a,l
	cp $fb
	jr z,.1
	cp $bb
	jr z,.1
	cp $7b
	jr nz,.6
.1:	ld de,ROW_WIDTH*BOX_HEIGHT-BOX_WIDTH*3
	add hl,de
.6:	ld a,l
	cp $14
	jp nz,.7

	call random
	ld a,l
	and $06
	ld l,a
	ld h,0
	ld de,corners
	add hl,de
	ld e,(hl)
	inc hl
	ld d,(hl)
	inc hl
	ex de,hl
	ld a,(page)
	add a,h
	ld h,a
	ld a,GR_LADDER
	call WRTVRM
	ex de,hl

	ld a,(level)
	cp YENDOR_LEVEL
	jr c,.11
	ld e,(hl)
	inc hl
	ld d,(hl)
	inc hl
	ex de,hl
	ld a,(page)
	add a,h
	ld h,a
	ld a,GR_YENDOR
	call WRTVRM
	ex de,hl
.11:
	; Switch to visible page, clear status bar
	ld hl,$0800
	ld bc,23*40
	xor a
	call FILVRM
	ld bc,$2107
	call WRTVDP
	ld a,VPAGE_VIS
	ld (page),a

	ld hl,19+(BOX_HEIGHT/2-1+BOX_HEIGHT)*ROW_WIDTH+$0800
	ld (hero),hl

game_loop:
	ld hl,(hero)
	ld de,-ROW_WIDTH-1
	add hl,de
	ld b,3
.1:	push hl
	call light
	inc hl
	call light
	inc hl
	call light
	pop hl
	ld de,ROW_WIDTH
	add hl,de
	djnz .1

	ld hl,(hero)
	call move_monsters

	ld hl,(hero)
	call RDVRM
	push af
	ld a,GR_HERO
	call WRTVRM
	push hl

	call update_hp

	ld a,(first)
	or a
	call nz,welcome

	call read_stick
	ld b,a

	pop hl
	pop af
	call WRTVRM

	ld a,b
	cp 1
	ld de,-40
	jr z,.2
	cp 3
	ld de,1
	jr z,.2
	cp 5
	ld de,40
	jr z,.2
	cp 7
	ld de,-1
	jp nz,game_loop
.2:
	add hl,de
	call RDVRM
	cp GR_LADDER
	jp z,ladder_found
	cp GR_DOOR
	jp z,move_over
	cp GR_FLOOR
	jp z,move_over
	cp GR_TUNNEL
	jp z,move_over
	jp nc,move_cancel
	cp GR_TRAP
	jp z,trap_found
	jp c,move_cancel
	cp GR_WEAPON+1
	jp nc,battle
	push af
	ld a,GR_FLOOR
	call WRTVRM
	set 2,h
	call WRTVRM
	res 2,h
	pop af
	cp GR_WEAPON
	jp z,weapon_found
	cp GR_ARMOR
	jp z,armor_found
	cp GR_FOOD
	jp z,food_found
	cp GR_GOLD
	jp z,gold_found
	cp GR_YENDOR
	jp z,amulet_found

move_cancel:
	jp game_loop

move_over:
	ld (hero),hl
	jp game_loop

show_message_all:
	call RDVRM
	push af
	push hl
	ld a,GR_HERO
	call WRTVRM
	push de
	call save_bar
	pop hl
	call show_message
	call wait_any_key		; any key dismisses message
	call restore_bar
	pop hl
	pop af
	call WRTVRM
	ret

amulet_found:
	ld de,.2
	call show_message_all
	ld a,$ff
	ld (yendor),a
	jp move_over
.2:	db "You found the Amulet of Yendor!!!",0

armor_found:
	ld de,.2
	call show_message_all
	ld a,(armor)
	inc a
	ld (armor),a
	jp move_over
.2:	db "You found some armor.",0

weapon_found:
	ld de,.2
	call show_message_all
	ld a,(weapon)
	inc a
	ld (weapon),a
	jp move_over
.2:	db "You found a better weapon.",0

gold_found:
	ld de,.2
	call show_message_all
	jp move_over
.2:	db "You found gold!",0

food_found:
	ld de,.2
	call show_message_all
	push hl
	call random
	ld a,l
	pop hl
.1:	sub 6
	jr nc,.1
	add a,7
	push hl
	ld l,a
	ld h,0
	call add_hp
	pop hl
	jp move_over
.2:	db "You found some food.",0

trap_found:
	ld de,.2
	call show_message_all
	push hl
	call random
	ld a,l
	pop hl
.1:	sub 6
	jr nc,.1
	add a,7
	neg
	push hl
	ld l,a
	ld h,$ff
	call add_hp
	pop hl
	jp move_over
.2:	db "Aaarghh!!! A trap.",0

battle:
	push hl
	and $1f
	ld (monster),a
	add a,a
	ld l,a
	ld h,0
	ld (monster_hp),hl
	ld (attack),a
.1:	call random
	ld a,(weapon)
	inc a
	ld b,a
	ld a,l
.2:	sub b
	jr nc,.2
	add a,b
	ld e,a
	ld d,0
	push de
	call announce_player_hit
	pop de
	ld hl,(monster_hp)
	or a
	sbc hl,de
	ld (monster_hp),hl
	jp c,.3
	call random
	ld a,(armor)
	ld c,a
	ld a,(attack)
	inc a
	ld b,a
	ld a,l
.4:	sub b
	jr nc,.4
	add a,b
	sub c
	jr nc,$+3
	xor a
	push af
	ld e,a
	ld d,0
	call announce_enemy_hit
	pop af
	or a
	jr z,.5
	neg
	ld l,a
	ld h,$ff
	call add_hp
	call update_hp
.5:	jp .1
.3:	pop hl
	ld a,GR_FLOOR
	call WRTVRM
	set 2,h
	call WRTVRM
	res 2,h
	jp move_over

add_hp:
	ex de,hl
	ld hl,(hp)
	add hl,de
	ld (hp),hl
	ld a,h
	or l
	jr z,.1
	bit 7,h
	ret z
.1:	ld hl,.2
	call show_message
	ld b,120
	call wait_vblank
	djnz $-3
	call wait_any_key
	jp title_screen
.2:	db "You are dead!",0

save_bar:
	ld hl,$0800
	ld de,temp
	ld b,40
.1:	call RDVRM
	ld (de),a
	inc de
	inc hl
	djnz .1
	ld hl,$0800
	ld bc,$0020
	xor a
	call FILVRM
	ret

restore_bar:
	ld hl,temp
	ld de,$0800
	ld bc,40
	jp LDIRVM

announce_enemy_hit:
	push de
	call save_bar
	ld hl,$0800
	ld a,$54
	call WRTVRM
	inc hl
	ld a,$68
	call WRTVRM
	inc hl
	ld a,$65
	call WRTVRM
	inc hl
	ld a,$20
	call WRTVRM
	inc hl
	ex de,hl
	ld a,(monster)
	add a,a
	ld c,a
	ld b,0
	ld hl,monster_list
	add hl,bc
	ld a,(hl)
	inc hl
	ld h,(hl)
	ld l,a
	call show_message2
	ex de,hl
	ex (sp),hl
	ex de,hl
	ld a,d
	or e
	jr z,.1
	ld a,(lfsr)
	and 6
	ld e,a
	ld d,0
	ld hl,.l0
	add hl,de
	jr .2
.1:	ld a,(lfsr)
	and 6
	ld e,a
	ld d,0
	ld hl,.l1
	add hl,de
.2:	ld a,(hl)
	inc hl
	ld h,(hl)
	ld l,a
	pop de
	call show_message2
	call wait_any_key		; Space/fire/any key advances
	jp restore_bar
.l0:	dw .m5,.m6,.m7,.m8
.l1:	dw .m1,.m2,.m3,.m4
.m1:	db " swings and misses you.",0
.m2:	db " misses you.",0
.m3:	db " barely misses you.",0
.m4:	db " doesn't hit you.",0
.m5:	db " hit you.",0
.m6:	db " hit you.",0
.m7:	db " has injured you.",0
.m8:	db " swings and hits you.",0

announce_player_hit:
	push de
	call save_bar
	pop de
	ld a,d
	or e
	jr z,.1
	ld a,(lfsr)
	and 6
	ld e,a
	ld d,0
	ld hl,.l0
	add hl,de
	jr .2
.1:	ld a,(lfsr)
	and 6
	ld e,a
	ld d,0
	ld hl,.l1
	add hl,de
.2:	ld a,(hl)
	inc hl
	ld h,(hl)
	ld l,a
	ld de,$0800
	call show_message
	ld a,(monster)
	add a,a
	ld c,a
	ld b,0
	ld hl,monster_list
	add hl,bc
	ld a,(hl)
	inc hl
	ld h,(hl)
	ld l,a
	call show_message2
	call wait_any_key		; Space/fire/any key advances
	jp restore_bar
.l0:	dw .m5,.m6,.m7,.m8
.l1:	dw .m1,.m2,.m3,.m4
.m1:	db "You swing and miss the ",0
.m2:	db "You miss the ",0
.m3:	db "You barely miss the ",0
.m4:	db "You don't hit the ",0
.m5:	db "You hit the ",0
.m6:	db "You hit the ",0
.m7:	db "You have injured the ",0
.m8:	db "You swing and hit the ",0

move_monsters:
	push hl
	set 2,h
	ld de,-ROW_WIDTH*2-2
	add hl,de
	ld de,temp
	ld b,5
.1:	push bc
	push hl
	ld b,5
.2:	ld a,h
	cp VPAGE_HID
	jr c,.0
	cp VPAGE_HID+4
	jr nc,.0
	call RDVRM
	jr .7
.0:	ld a,'.'
.7:	ld (de),a
	inc de
	inc hl
	djnz .2
	pop hl
	ld bc,ROW_WIDTH
	add hl,bc
	pop bc
	djnz .1
	ld hl,temp
	ld de,temp+6
	call move_monster
	ld hl,temp+1
	ld de,temp+6
	call move_monster
	ld hl,temp+5
	ld de,temp+6
	call move_monster
	ld hl,temp+1
	ld de,temp+7
	call move_monster
	ld hl,temp+2
	ld de,temp+7
	call move_monster
	ld hl,temp+3
	ld de,temp+7
	call move_monster
	ld hl,temp+3
	ld de,temp+8
	call move_monster
	ld hl,temp+4
	ld de,temp+8
	call move_monster
	ld hl,temp+9
	ld de,temp+8
	call move_monster
	ld hl,temp+5
	ld de,temp+11
	call move_monster
	ld hl,temp+10
	ld de,temp+11
	call move_monster
	ld hl,temp+15
	ld de,temp+11
	call move_monster
	ld hl,temp+9
	ld de,temp+13
	call move_monster
	ld hl,temp+14
	ld de,temp+13
	call move_monster
	ld hl,temp+19
	ld de,temp+13
	call move_monster
	ld hl,temp+15
	ld de,temp+16
	call move_monster
	ld hl,temp+20
	ld de,temp+16
	call move_monster
	ld hl,temp+21
	ld de,temp+16
	call move_monster
	ld hl,temp+21
	ld de,temp+17
	call move_monster
	ld hl,temp+22
	ld de,temp+17
	call move_monster
	ld hl,temp+23
	ld de,temp+17
	call move_monster
	ld hl,temp+19
	ld de,temp+18
	call move_monster
	ld hl,temp+23
	ld de,temp+18
	call move_monster
	ld hl,temp+24
	ld de,temp+18
	call move_monster
	pop hl
	push hl
	set 2,h
	ld de,-82
	add hl,de
	ld de,temp
	ld b,5
.3:	push bc
	push hl
	ld b,5
.4:	ld a,h
	cp VPAGE_HID
	jr c,.6
	cp VPAGE_HID+4
	jr nc,.6
	ld a,(de)
	call WRTVRM
	res 2,h
	call RDVRM
	or a
	jr z,.5
	ld a,(de)
	or a
	jr z,.5
	call WRTVRM
.5:	set 2,h
.6:	inc de
	inc hl
	djnz .4
	pop hl
	ld bc,ROW_WIDTH
	add hl,bc
	pop bc
	djnz .3
	pop hl
	ret

move_monster:
	ld a,(de)
	cp GR_FLOOR
	ret nz
	ld a,(hl)
	push hl
	ld hl,.1
	ld bc,16
	cpir
	pop hl
	ret nz
	push hl
	call random
	ld a,l
	and $54
	pop hl
	ret nz
	ld a,(hl)
	ld (de),a
	ld (hl),GR_FLOOR
	ret
.1:	db "BCEF"
	db "HILN"
	db "PQRS"
	db "TUVZ",0

monster_list:
	dw 0
	dw .m1,.m2,.m3,.m4,.m5,.m6,.m7,.m8,.m9,.m10
	dw .m11,.m12,.m13,.m14,.m15,.m16,.m17,.m18,.m19,.m20
	dw .m21,.m22,.m23,.m24,.m25,.m26
.m1:	db "aardvark",0
.m2:	db "bee",0
.m3:	db "crocodile",0
.m4:	db "demon",0
.m5:	db "elf",0
.m6:	db "fairy",0
.m7:	db "goober",0
.m8:	db "hanta",0
.m9:	db "ilkor",0
.m10:	db "jabba",0
.m11:	db "kelkor",0
.m12:	db "lycan",0
.m13:	db "mole",0
.m14:	db "number",0
.m15:	db "okapi",0
.m16:	db "proteus",0
.m17:	db "quagga",0
.m18:	db "rancor",0
.m19:	db "snake",0
.m20:	db "trantor",0
.m21:	db "unicorn",0
.m22:	db "valkor",0
.m23:	db "werken",0
.m24:	db "xantor",0
.m25:	db "manquark",0
.m26:	db "zombie master",0

show_message:
	ld de,$0800
show_message2:
.1:	ld a,(hl)
	or a
	ret z
	ex de,hl
	call WRTVRM
	ex de,hl
	inc de
	inc hl
	jr .1

update_level:
	ld hl,.1
	ld de,$0800+23*ROW_WIDTH+1
	ld bc,7
	call LDIRVM
	ld hl,$0800+23*ROW_WIDTH+10
	exx
	ld a,(level)
	ld l,a
	ld h,0
	exx
	jp show_number
.1:	db "Level: "

update_hp:
	ld hl,$0800+23*ROW_WIDTH+38
	exx
	ld hl,(hp)
	exx
	jp show_number

show_number:
.2:	exx
	ld de,10
	ld bc,-1
.1:	inc bc
	or a
	sbc hl,de
	jr nc,.1
	add hl,de
	ld a,l
	add a,$30
	exx
	call WRTVRM
	dec hl
	exx
	ld h,b
	ld l,c
	ld a,b
	or c
	exx
	jp nz,.2
	ld a,$20
	jp WRTVRM

ladder_found:
	jp generate_dungeon

light:
	set 2,h		; read from hidden page ($0C00)
	call RDVRM
	res 2,h		; write to visible page ($0800)
	jp WRTVRM

	; wait_vblank: poll VDP status bit 7 for VBLANK
	; Also updates ticks and stirs LFSR for randomness
wait_vblank:
    if NABUCPM
	; Under CP/M the BIOS reads VDP status in its own ISR, consuming
	; bit 7 before we see it. Use a counted delay instead (~1 frame).
	push bc
	ld bc,4000
.dly:	dec bc
	ld a,b
	or c
	jr nz,.dly
	pop bc
    endif
    if 1-NABUCPM
.wait:	in a,(VDPIO)
	and $80
	jr z,.wait
    endif
	; Stir LFSR and tick
	ld hl,(ticks)
	inc hl
	ld (ticks),hl
	ld de,(lfsr)
	add hl,de
	ld (lfsr),hl
	; Poll keyboard/joystick via direct hardware port
	; Same under bare-metal and CP/M - the 8251A UART is at $90/$91
	; regardless of whether CP/M BIOS is present.
	in a,(KBDSTA)
	and $02			; 8251A RXRDY = bit 1
	ret z
	in a,(KBDDAT)
	; Filter watchdog and key-up events first (all have bit7 set)
	cp $94			; watchdog - discard
	jr z,.nokey
	; Arrow key-up events - discard (we act on key-down only)
	cp $F0			; right arrow up
	jr z,.nokey
	cp $F1			; left arrow up
	jr z,.nokey
	cp $F2			; up arrow up
	jr z,.nokey
	cp $F3			; down arrow up
	jr z,.nokey
	; Arrow key-down events
	cp $E0			; right arrow down
	jr z,.arrow_right
	cp $E1			; left arrow down
	jr z,.arrow_left
	cp $E2			; up arrow down
	jr z,.arrow_up
	cp $E3			; down arrow down
	jr z,.arrow_down
	; Remaining bit-7-set bytes:
	; $80-$BF = joystick
	; $C0-$FF = key-up events for regular keys (ASCII | $80) - discard
	bit 7,a
	jr z,.ascii		; bit 7 clear = regular ASCII
	cp $C0			; >= $C0 = key-up for regular key - discard
	jr nc,.nokey
	jr .joystick		; $80-$BF = joystick
.ascii:
	; Regular keyboard ASCII (bit 7 clear)
	cp 13			; Enter
	jr z,.store
	cp ' '			; ignore other control bytes
	jr c,.nokey
.store:	ld (kbd_char),a		; always overwrite - debounce handles rate limiting
.nokey:	ret
.joystick:
	; NABU joystick byte format (bit7=1):
	; $80=center $81=up $82=right $84=down $88=left
	; $91=up+fire $92=right+fire $94=down+fire $98=left+fire
	; $A0/$B0 = joystick 2
	; Fire button alone: $90 (joy1) $B0 (joy2)
	and $1F			; mask out joystick ID bits
	or a
	jr z,.nokey		; $80 = center, ignore
	cp $10			; $90 = fire only -> Space
	jr z,.fire_only
	and $0F			; isolate direction (ignore fire bit)
	cp $01			; $01 = left
	jr z,.joy_up
	cp $02			; $02 = down
	jr z,.joy_right
	cp $04			; $04 = right
	jr z,.joy_down
	cp $08			; $08 = up
	jr z,.joy_left
	jr .nokey
.arrow_up:
	ld a,'w'
	jr .store
.arrow_down:
	ld a,'s'
	jr .store
.arrow_left:
	ld a,'a'
	jr .store
.arrow_right:
	ld a,'d'
	jr .store
.fire_only:
	ld a,' '
	jr .store
.joy_up:
	ld a,'a'
	jr .store
.joy_right:
	ld a,'s'
	jr .store
.joy_down:
	ld a,'d'
	jr .store
.joy_left:
	ld a,'w'
	jr .store

	; wait_any_key: wait for any keypress or joystick button to start game
	; Joystick directions count; joystick movements do NOT (handled by
	; the wait_vblank keyboard filter - joystick moves store wasd chars).
	; Actually we want ANY stored kbd_char to count - including wasd.
	; Only pure joystick movement bytes that weren't converted count too.
	; Simplest: return when kbd_char is nonzero, then clear it.
wait_any_key:
	call wait_vblank
	ld a,(kbd_char)
	or a
	jr z,wait_any_key
	xor a
	ld (kbd_char),a		; consume the key
	ld a,30			; set debounce so first game move needs a fresh press
	ld (debounce),a
	ret

read_stick:
	call wait_vblank
	ld a,(debounce)
	or a
	jr z,.1
	dec a
	ld (debounce),a
	jr read_stick
.1:	push hl
	xor a
	call GTSTCK
	pop hl
	or a
	jr nz,.2
	push hl
	xor a
	call GTTRIG
	pop hl
	or a
	jr z,read_stick
	ld a,10
	ld (debounce),a
	; Clear kbd_char so key-repeat during debounce doesn't re-trigger
	xor a
	ld (kbd_char),a
	ld a,$ff
	ret
.2:	push af
	ld a,10
	ld (debounce),a
	; Clear kbd_char so key-repeat during debounce doesn't queue another move
	xor a
	ld (kbd_char),a
	pop af
	cp 1
	ret z
	cp 3
	ret z
	cp 5
	ret z
	cp 7
	ret z
	xor a
	ret

game_won:
	ld hl,.1
	call show_message
	ld b,120
	call wait_vblank
	djnz $-3
	call wait_any_key
	jp title_screen
.1:	db "You have made it to the surface!",0

welcome:
	xor a
	ld (first),a
	ld hl,.1
	call show_message
	call wait_any_key
	ld hl,$0800
	ld bc,32
	xor a
	jp FILVRM
.1:	db "Welcome to the dungeons of doom!",0

corners:
	dw (BOX_HEIGHT/2-1)*ROW_WIDTH+(BOX_WIDTH/2)
	dw (BOX_HEIGHT/2-1)*ROW_WIDTH+(BOX_WIDTH/2)+BOX_WIDTH*2
	dw (BOX_HEIGHT/2-1+BOX_HEIGHT*2)*ROW_WIDTH+(BOX_WIDTH/2)
	dw (BOX_HEIGHT/2-1+BOX_HEIGHT*2)*ROW_WIDTH+(BOX_WIDTH/2)+BOX_WIDTH*2
	dw (BOX_HEIGHT/2-1)*ROW_WIDTH+(BOX_WIDTH/2)

	; ── VDP subroutines ─────────────────────────────────────────────
	; All use push bc/pop bc delay as required by TMS9918A

	; SETRD: set VDP VRAM address for reading
SETRD:
    if 1-NABUCPM
	di
    endif
	ld a,l
	out (VDPIO),a
	push bc
	pop bc
	ld a,h
	and $3f
	out (VDPIO),a
	push bc
	pop bc
    if 1-NABUCPM
	ei
    endif
	ret

	; SETWRT: set VDP VRAM address for writing
SETWRT:
    if 1-NABUCPM
	di
    endif
	ld a,l
	out (VDPIO),a
	push bc
	pop bc
	ld a,h
	or $40
	out (VDPIO),a
	push bc
	pop bc
    if 1-NABUCPM
	ei
    endif
	ret

	; WRTVDP: write VDP register  B=value  C=register
WRTVDP:
    if 1-NABUCPM
	di
    endif
	ld a,b
	out (VDPIO),a
	push bc
	pop bc
	ld a,c
	or $80
	out (VDPIO),a
	push bc
	pop bc
    if 1-NABUCPM
	ei
    endif
	ret

	; RDVRM: read byte from VRAM  HL=address -> A
RDVRM:
	call SETRD
	in a,(VDPRAM)
	ret

	; WRTVRM: write byte to VRAM  HL=address  A=byte
WRTVRM:
	push af
	call SETWRT
	pop af
	out (VDPRAM),a
	push bc
	pop bc
	ret

	; FILVRM: fill VRAM  HL=start  BC=count  A=fill byte
FILVRM:
	ld d,a			; save fill byte in D (SETWRT only touches A, not D)
	call SETWRT		; set VDP write address from HL
.1:	ld a,d			; reload fill byte every iteration
	out (VDPRAM),a
	push bc
	pop bc
	dec bc
	ld a,b
	or c
	jr nz,.1
	ret

	; LDIRVM: copy RAM->VRAM  HL=src  DE=dst  BC=count
LDIRVM:
	ex de,hl
	call SETWRT
	ex de,hl
.1:	ld a,(hl)
	out (VDPRAM),a
	push bc
	pop bc
	inc hl
	dec bc
	ld a,b
	or c
	jr nz,.1
	ret

	; GTSTCK: read direction from keyboard  returns 0/1/3/5/7
GTSTCK:
	ld a,(kbd_char)
	or a
	jr z,.none
	cp 'w'
	jr z,.up
	cp 'W'
	jr z,.up
	cp 'd'
	jr z,.right
	cp 'D'
	jr z,.right
	cp 's'
	jr z,.down
	cp 'S'
	jr z,.down
	cp 'a'
	jr z,.left
	cp 'A'
	jr z,.left
.none:	xor a
	ret
.up:	xor a
	ld (kbd_char),a
	ld a,1
	ret
.right:	xor a
	ld (kbd_char),a
	ld a,3
	ret
.down:	xor a
	ld (kbd_char),a
	ld a,5
	ret
.left:	xor a
	ld (kbd_char),a
	ld a,7
	ret

	; GTTRIG: fire on any key that isn't a direction key
GTTRIG:
	ld a,(kbd_char)
	or a
	ret z
	; Direction keys belong to GTSTCK - return 0 without consuming
	cp 'w'
	ret z
	cp 'W'
	ret z
	cp 'a'
	ret z
	cp 'A'
	ret z
	cp 's'
	ret z
	cp 'S'
	ret z
	cp 'd'
	ret z
	cp 'D'
	ret z
	; Everything else: consume and return fire
.fire:	xor a
	ld (kbd_char),a
	ld a,$ff
	ret

	; random: 16-bit Galois LFSR
random:
	push bc
	ld hl,(lfsr)
	ld a,h
	or l
	jr nz,.0
	ld hl,$7811
.0:	ld a,h
	and $80
	ld b,a
	ld a,h
	and $02
	rrca
	rrca
	xor b
	ld b,a
	ld a,h
	and $01
	rrca
	xor b
	ld b,a
	ld a,l
	and $20
	rlca
	rlca
	xor b
	rlca
	rr h
	rr l
	ld (lfsr),hl
	pop bc
	ret

fill:	push hl
	ld a,ixl
	call door
	ld a,(box_w)
	inc a
	ld b,a
.1:	ld a,iyl
	call door
	djnz .1
	ld a,iyh
	call door
	pop hl
	ld de,ROW_WIDTH
	add hl,de
	ret

door:
	cp GR_FLOOR
	jr nz,.3
	push af
	push hl
	call random
	ld a,l
	and $3f
	cp 5
	jr nc,.4
	ld c,a
	ld a,(level)
	add a,c
	dec a
.6:	cp $1a
	jr c,.5
	sub 5
	jr .6
.5:	add a,$41
	pop hl
	pop de
	jr .3
.4:	cp 14
	jr nc,.7
	ld hl,items-5
	ld e,a
	ld d,0
	add hl,de
	ld a,(hl)
	pop hl
	pop de
	jr .3
.7:	pop hl
	pop af
.3:	cp GR_HORIZ
	jr z,.1
	cp GR_VERT
	jr nz,.2
.1:	ld c,a
	call RDVRM
	cp GR_TUNNEL
	ld a,c
	jr nz,.2
	ld a,GR_DOOR
.2:	call WRTVRM
	inc hl
	ret

items:
	db GR_FOOD,GR_TRAP,GR_FOOD,GR_ARMOR
	db GR_GOLD,GR_WEAPON,GR_FOOD,GR_GOLD,GR_GOLD


test:
	db "OSCAR WAS HERE ",$01

title_letters:
	db "     _______                            "
	db "    | _ | _ |                           "
	db "  ___\\V/||/'|_ __ ___   __ _ _   _  ___ "
	db " |_ //_\\|/ || '__/ _ \\ / _` | | | |/ _ \\"
	db "  //||_|\\|_// | | (_) | (_| | |_| |  __/"
	db " /__\\___/\\_/|_|  \\___/ \\__, |\\__,_|\\___|"
	db " \\        A________     __/ |          /"
	db "  \\    )==o________>   |___/          / "
	db "   \\______V__________________________/  "
	db "                                        "
	db "           by Oscar Toledo G.           "
	db "                                        "
	db "                                        "
	db "           NABU port by GTAMP           "
	db "                                        "
	db "                                        "
	db "                                        "
	db "         Press button to start          "

letters_bitmaps:
	db $00,$00,$00,$00,$00,$00,$00,$00	; $00
	db $78,$fc,$b4,$fc,$fc,$b4,$84,$78	; $01 happy face
	db $00,$00,$00,$00,$00,$00,$00,$00	; $02
	db $00,$00,$00,$00,$00,$00,$00,$00	; $03
	db $00,$20,$70,$f8,$70,$20,$00,$00	; $04
	db $30,$30,$d8,$d8,$30,$30,$78,$00	; $05
	db $00,$00,$00,$00,$00,$00,$00,$00	; $06
	db $00,$00,$00,$00,$00,$00,$00,$00	; $07
	db $fc,$fc,$ec,$c4,$c4,$ec,$fc,$fc	; $08
	db $00,$00,$00,$00,$00,$00,$00,$00	; $09
	db $00,$00,$00,$00,$00,$00,$00,$00	; $0a
	db $00,$00,$00,$00,$00,$00,$00,$00	; $0b
	db $70,$88,$70,$20,$f8,$20,$20,$00	; $0c
	db $00,$00,$00,$00,$00,$00,$00,$00	; $0d
	db $00,$00,$00,$00,$00,$00,$00,$00	; $0e
	db $20,$a8,$70,$d8,$70,$a8,$20,$00	; $0f
	db $00,$00,$00,$00,$00,$00,$00,$00	; $10
	db $00,$00,$00,$00,$00,$00,$00,$00	; $11
	db $00,$00,$00,$00,$00,$00,$00,$00	; $12
	db $00,$00,$00,$00,$00,$00,$00,$00	; $13
	db $00,$00,$00,$00,$00,$00,$00,$00	; $14
	db $00,$00,$00,$00,$00,$00,$00,$00	; $15
	db $00,$00,$00,$00,$00,$00,$00,$00	; $16
	db $00,$00,$00,$00,$00,$00,$00,$00	; $17
	db $20,$70,$f8,$20,$20,$20,$20,$00	; $18
	db $00,$00,$00,$00,$00,$00,$00,$00	; $19
	db $00,$00,$00,$00,$00,$00,$00,$00	; $1a
	db $00,$00,$00,$00,$00,$00,$00,$00	; $1b
	db $00,$00,$00,$00,$00,$00,$00,$00	; $1c
	db $00,$00,$00,$00,$00,$00,$00,$00	; $1d
	db $00,$00,$00,$00,$00,$00,$00,$00	; $1e
	db $00,$00,$00,$00,$00,$00,$00,$00	; $1f
	db $00,$00,$00,$00,$00,$00,$00,$00	; $20
	db $20,$20,$20,$20,$20,$00,$20,$00	; $21
	db $50,$50,$00,$00,$00,$00,$00,$00	; $22
	db $50,$50,$f8,$50,$f8,$50,$50,$00	; $23
	db $20,$78,$a0,$70,$28,$f0,$20,$00	; $24
	db $c0,$c8,$10,$20,$40,$98,$98,$00	; $25
	db $60,$90,$60,$90,$94,$98,$68,$00	; $26
	db $20,$20,$40,$00,$00,$00,$00,$00	; $27
	db $08,$10,$20,$20,$20,$10,$08,$00	; $28
	db $80,$40,$20,$20,$20,$40,$80,$00	; $29
	db $00,$20,$20,$f8,$50,$88,$00,$00	; $2a
	db $00,$20,$20,$f8,$20,$20,$00,$00	; $2b
	db $00,$00,$00,$00,$00,$30,$10,$20	; $2c
	db $00,$00,$00,$f8,$00,$00,$00,$00	; $2d
	db $00,$00,$00,$00,$00,$30,$30,$00	; $2e
	db $00,$08,$10,$20,$40,$80,$00,$00	; $2f
	db $70,$88,$98,$a8,$c8,$88,$70,$00	; $30
	db $20,$60,$20,$20,$20,$20,$70,$00	; $31
	db $70,$88,$10,$20,$40,$80,$f8,$00	; $32
	db $70,$88,$08,$30,$08,$88,$70,$00	; $33
	db $10,$30,$50,$90,$f8,$10,$10,$00	; $34
	db $f8,$80,$f0,$08,$08,$08,$f0,$00	; $35
	db $78,$80,$80,$f0,$88,$88,$70,$00	; $36
	db $f8,$08,$08,$10,$20,$20,$20,$00	; $37
	db $70,$88,$88,$70,$88,$88,$70,$00	; $38
	db $70,$88,$88,$78,$08,$88,$70,$00	; $39
	db $00,$30,$30,$00,$30,$30,$00,$00	; $3a
	db $00,$30,$30,$00,$30,$30,$10,$20	; $3b
	db $00,$18,$60,$80,$60,$18,$00,$00	; $3c
	db $00,$00,$f8,$00,$f8,$00,$00,$00	; $3d
	db $00,$c0,$30,$08,$30,$c0,$00,$00	; $3e
	db $70,$88,$08,$10,$20,$00,$20,$00	; $3f
	db $70,$88,$98,$a8,$98,$80,$78,$00	; $40
	db $20,$50,$88,$88,$f8,$88,$88,$00	; $41
	db $f0,$88,$88,$f0,$88,$88,$f0,$00	; $42
	db $70,$88,$80,$80,$80,$88,$70,$00	; $43
	db $f0,$88,$88,$88,$88,$88,$f0,$00	; $44
	db $f8,$80,$80,$f0,$80,$80,$f8,$00	; $45
	db $f8,$80,$80,$f0,$80,$80,$80,$00	; $46
	db $70,$88,$80,$98,$88,$88,$70,$00	; $47
	db $88,$88,$88,$f8,$88,$88,$88,$00	; $48
	db $70,$20,$20,$20,$20,$20,$70,$00	; $49
	db $08,$08,$08,$08,$88,$88,$70,$00	; $4a
	db $88,$90,$a0,$c0,$a0,$90,$88,$00	; $4b
	db $80,$80,$80,$80,$80,$80,$f8,$00	; $4c
	db $88,$d8,$a8,$a8,$88,$88,$88,$00	; $4d
	db $88,$88,$c8,$a8,$98,$88,$88,$00	; $4e
	db $70,$88,$88,$88,$88,$88,$70,$00	; $4f
	db $f0,$88,$88,$f0,$80,$80,$80,$00	; $50
	db $70,$88,$88,$88,$88,$a8,$70,$08	; $51
	db $f0,$88,$88,$f0,$a0,$90,$88,$00	; $52
	db $78,$80,$80,$70,$08,$08,$f0,$00	; $53
	db $f8,$20,$20,$20,$20,$20,$20,$00	; $54
	db $88,$88,$88,$88,$88,$88,$70,$00	; $55
	db $88,$88,$88,$88,$88,$50,$20,$00	; $56
	db $88,$88,$88,$88,$a8,$a8,$50,$00	; $57
	db $88,$88,$50,$20,$50,$88,$88,$00	; $58
	db $88,$88,$50,$20,$20,$20,$20,$00	; $59
	db $f8,$08,$10,$20,$40,$80,$f8,$00	; $5a
	db $70,$60,$60,$60,$60,$60,$70,$00	; $5b
	db $00,$80,$40,$20,$10,$08,$00,$00	; $5c
	db $70,$30,$30,$30,$30,$30,$70,$00	; $5d
	db $20,$50,$88,$00,$00,$00,$00,$00	; $5e
	db $00,$00,$00,$00,$00,$00,$00,$fc	; $5f
	db $20,$20,$40,$00,$00,$00,$00,$00	; $60
	db $00,$00,$68,$98,$88,$98,$68,$00	; $61
	db $80,$80,$f0,$88,$88,$88,$f0,$00	; $62
	db $00,$00,$78,$80,$80,$80,$78,$00	; $63
	db $08,$08,$68,$98,$88,$98,$68,$00	; $64
	db $00,$00,$70,$88,$f8,$80,$70,$00	; $65
	db $18,$20,$20,$70,$20,$20,$70,$00	; $66
	db $00,$00,$70,$88,$88,$78,$08,$70	; $67
	db $80,$80,$b0,$c8,$88,$88,$88,$00	; $68
	db $20,$00,$60,$20,$20,$20,$70,$00	; $69
	db $00,$00,$08,$08,$88,$88,$70,$00	; $6a
	db $80,$80,$90,$a0,$e0,$90,$88,$00	; $6b
	db $60,$20,$20,$20,$20,$20,$70,$00	; $6c
	db $00,$00,$d0,$a8,$a8,$a8,$a8,$00	; $6d
	db $00,$00,$b0,$c8,$88,$88,$88,$00	; $6e
	db $00,$00,$70,$88,$88,$88,$70,$00	; $6f
	db $00,$00,$b0,$c8,$c8,$b0,$80,$80	; $70
	db $00,$00,$68,$98,$98,$68,$08,$08	; $71
	db $00,$00,$b0,$c8,$80,$80,$80,$00	; $72
	db $00,$00,$78,$80,$70,$08,$f0,$00	; $73
	db $20,$20,$f8,$20,$20,$20,$18,$00	; $74
	db $00,$00,$88,$88,$88,$98,$68,$00	; $75
	db $00,$00,$88,$88,$88,$50,$20,$00	; $76
	db $00,$00,$88,$a8,$a8,$a8,$50,$00	; $77
	db $00,$00,$88,$50,$20,$50,$88,$00	; $78
	db $00,$00,$88,$88,$98,$68,$08,$f0	; $79
	db $00,$00,$f8,$10,$20,$40,$f8,$00	; $7a
	db $30,$40,$40,$20,$40,$40,$30,$00	; $7b
	db $20,$20,$20,$20,$20,$20,$20,$20	; $7c
	db $60,$10,$10,$20,$10,$10,$60,$00	; $7d
	db $40,$a8,$10,$00,$00,$00,$00,$00	; $7e
	db $00,$00,$70,$88,$88,$f8,$00,$00	; $7f
	db $00,$00,$00,$00,$00,$00,$00,$00	; $80
	db $00,$00,$00,$00,$00,$00,$00,$00	; $81
	db $00,$00,$00,$00,$00,$00,$00,$00	; $82
	db $00,$00,$00,$00,$00,$00,$00,$00	; $83
	db $00,$00,$00,$00,$00,$00,$00,$00	; $84
	db $00,$00,$00,$00,$00,$00,$00,$00	; $85
	db $00,$00,$00,$00,$00,$00,$00,$00	; $86
	db $00,$00,$00,$00,$00,$00,$00,$00	; $87
	db $00,$00,$00,$00,$00,$00,$00,$00	; $88
	db $00,$00,$00,$00,$00,$00,$00,$00	; $89
	db $00,$00,$00,$00,$00,$00,$00,$00	; $8a
	db $00,$00,$00,$00,$00,$00,$00,$00	; $8b
	db $00,$00,$00,$00,$00,$00,$00,$00	; $8c
	db $00,$00,$00,$00,$00,$00,$00,$00	; $8d
	db $00,$00,$00,$00,$00,$00,$00,$00	; $8e
	db $00,$00,$00,$00,$00,$00,$00,$00	; $8f
	db $00,$00,$00,$00,$00,$00,$00,$00	; $90
	db $00,$00,$00,$00,$00,$00,$00,$00	; $91
	db $00,$00,$00,$00,$00,$00,$00,$00	; $92
	db $00,$00,$00,$00,$00,$00,$00,$00	; $93
	db $00,$00,$00,$00,$00,$00,$00,$00	; $94
	db $00,$00,$00,$00,$00,$00,$00,$00	; $95
	db $00,$00,$00,$00,$00,$00,$00,$00	; $96
	db $00,$00,$00,$00,$00,$00,$00,$00	; $97
	db $00,$00,$00,$00,$00,$00,$00,$00	; $98
	db $00,$00,$00,$00,$00,$00,$00,$00	; $99
	db $00,$00,$00,$00,$00,$00,$00,$00	; $9a
	db $00,$00,$00,$00,$00,$00,$00,$00	; $9b
	db $00,$00,$00,$00,$00,$00,$00,$00	; $9c
	db $00,$00,$00,$00,$00,$00,$00,$00	; $9d
	db $00,$00,$00,$00,$00,$00,$00,$00	; $9e
	db $00,$00,$00,$00,$00,$00,$00,$00	; $9f
	db $00,$00,$00,$00,$00,$00,$00,$00	; $a0
	db $00,$00,$00,$00,$00,$00,$00,$00	; $a1
	db $00,$00,$00,$00,$00,$00,$00,$00	; $a2
	db $00,$00,$00,$00,$00,$00,$00,$00	; $a3
	db $00,$00,$00,$00,$00,$00,$00,$00	; $a4
	db $00,$00,$00,$00,$00,$00,$00,$00	; $a5
	db $00,$00,$00,$00,$00,$00,$00,$00	; $a6
	db $00,$00,$00,$00,$00,$00,$00,$00	; $a7
	db $00,$00,$00,$00,$00,$00,$00,$00	; $a8
	db $00,$00,$00,$00,$00,$00,$00,$00	; $a9
	db $00,$00,$00,$00,$00,$00,$00,$00	; $aa
	db $00,$00,$00,$00,$00,$00,$00,$00	; $ab
	db $00,$00,$00,$00,$00,$00,$00,$00	; $ac
	db $00,$00,$00,$00,$00,$00,$00,$00	; $ad
	db $00,$00,$00,$00,$00,$00,$00,$00	; $ae
	db $00,$00,$00,$00,$00,$00,$00,$00	; $af
	db $00,$00,$00,$00,$00,$00,$00,$00	; $b0
	db $a8,$54,$a8,$54,$a8,$54,$a8,$54	; $b1
	db $00,$00,$00,$00,$00,$00,$00,$00	; $b2
	db $00,$00,$00,$00,$00,$00,$00,$00	; $b3
	db $00,$00,$00,$00,$00,$00,$00,$00	; $b4
	db $00,$00,$00,$00,$00,$00,$00,$00	; $b5
	db $00,$00,$00,$00,$00,$00,$00,$00	; $b6
	db $00,$00,$00,$00,$00,$00,$00,$00	; $b7
	db $00,$00,$00,$00,$00,$00,$00,$00	; $b8
	db $00,$00,$00,$00,$00,$00,$00,$00	; $b9
	db $28,$28,$28,$28,$28,$28,$28,$28	; $ba
	db $00,$00,$f8,$08,$e8,$28,$28,$28	; $bb
	db $28,$28,$e8,$08,$f8,$00,$00,$00	; $bc
	db $00,$00,$00,$00,$00,$00,$00,$00	; $bd
	db $00,$00,$00,$00,$00,$00,$00,$00	; $be
	db $00,$00,$00,$00,$00,$00,$00,$00	; $bf
	db $00,$00,$00,$00,$00,$00,$00,$00	; $c0
	db $00,$00,$00,$00,$00,$00,$00,$00	; $c1
	db $00,$00,$00,$00,$00,$00,$00,$00	; $c2
	db $00,$00,$00,$00,$00,$00,$00,$00	; $c3
	db $00,$00,$00,$00,$00,$00,$00,$00	; $c4
	db $00,$00,$00,$00,$00,$00,$00,$00	; $c5
	db $00,$00,$00,$00,$00,$00,$00,$00	; $c6
	db $00,$00,$00,$00,$00,$00,$00,$00	; $c7
	db $28,$28,$2c,$20,$3c,$00,$00,$00	; $c8
	db $00,$00,$3c,$20,$2c,$28,$28,$28	; $c9
	db $00,$00,$00,$00,$00,$00,$00,$00	; $ca
	db $00,$00,$00,$00,$00,$00,$00,$00	; $cb
	db $00,$00,$00,$00,$00,$00,$00,$00	; $cc
	db $00,$00,$fc,$00,$fc,$00,$00,$00	; $cd
	db $28,$28,$ec,$00,$ec,$28,$28,$28	; $ce
	db $00,$00,$00,$00,$00,$00,$00,$00	; $cf
	db $00,$00,$00,$00,$00,$00,$00,$00	; $d0
	db $00,$00,$00,$00,$00,$00,$00,$00	; $d1
	db $00,$00,$00,$00,$00,$00,$00,$00	; $d2
	db $00,$00,$00,$00,$00,$00,$00,$00	; $d3
	db $00,$00,$00,$00,$00,$00,$00,$00	; $d4
	db $00,$00,$00,$00,$00,$00,$00,$00	; $d5
	db $00,$00,$00,$00,$00,$00,$00,$00	; $d6
	db $00,$00,$00,$00,$00,$00,$00,$00	; $d7
	db $00,$00,$00,$00,$00,$00,$00,$00	; $d8
	db $00,$00,$00,$00,$00,$00,$00,$00	; $d9
	db $00,$00,$00,$00,$00,$00,$00,$00	; $da
	db $00,$00,$00,$00,$00,$00,$00,$00	; $db
	db $00,$00,$00,$00,$00,$00,$00,$00	; $dc
	db $00,$00,$00,$00,$00,$00,$00,$00	; $dd
	db $00,$00,$00,$00,$00,$00,$00,$00	; $de
	db $00,$00,$00,$00,$00,$00,$00,$00	; $df
	db $00,$00,$00,$00,$00,$00,$00,$00	; $e0
	db $00,$00,$00,$00,$00,$00,$00,$00	; $e1
	db $00,$00,$00,$00,$00,$00,$00,$00	; $e2
	db $00,$00,$00,$00,$00,$00,$00,$00	; $e3
	db $00,$00,$00,$00,$00,$00,$00,$00	; $e4
	db $00,$00,$00,$00,$00,$00,$00,$00	; $e5
	db $00,$00,$00,$00,$00,$00,$00,$00	; $e6
	db $00,$00,$00,$00,$00,$00,$00,$00	; $e7
	db $00,$00,$00,$00,$00,$00,$00,$00	; $e8
	db $00,$00,$00,$00,$00,$00,$00,$00	; $e9
	db $00,$00,$00,$00,$00,$00,$00,$00	; $ea
	db $00,$00,$00,$00,$00,$00,$00,$00	; $eb
	db $00,$00,$00,$00,$00,$00,$00,$00	; $ec
	db $00,$00,$00,$00,$00,$00,$00,$00	; $ed
	db $00,$00,$00,$00,$00,$00,$00,$00	; $ee
	db $00,$00,$00,$00,$00,$00,$00,$00	; $ef
	db $fc,$84,$fc,$84,$fc,$84,$fc,$fc	; $f0
	db $00,$00,$00,$00,$00,$00,$00,$00	; $f1
	db $00,$00,$00,$00,$00,$00,$00,$00	; $f2
	db $00,$00,$00,$00,$00,$00,$00,$00	; $f3
	db $00,$00,$00,$00,$00,$00,$00,$00	; $f4
	db $00,$00,$00,$00,$00,$00,$00,$00	; $f5
	db $00,$00,$00,$00,$00,$00,$00,$00	; $f6
	db $00,$00,$00,$00,$00,$00,$00,$00	; $f7
	db $00,$00,$00,$00,$00,$00,$00,$00	; $f8
	db $00,$00,$00,$00,$00,$00,$00,$00	; $f9
	db $00,$00,$00,$20,$00,$00,$00,$00	; $fa
	db $00,$00,$00,$00,$00,$00,$00,$00	; $fb
	db $00,$00,$00,$00,$00,$00,$00,$00	; $fc
	db $00,$00,$00,$00,$00,$00,$00,$00	; $fd
	db $00,$00,$00,$00,$00,$00,$00,$00	; $fe
	db $00,$00,$00,$00,$00,$00,$00,$00	; $ff

	; ── Variables ──────────────────────────────────────────────────
    if NABUCPM
	org $3000		; CP/M: well inside TPA, below typical BDOS
    endif
    if 1-NABUCPM
	org $F000		; Bare-metal: upper RAM
    endif

kbd_char:	rb 1
ticks:		rb 2
page:		rb 1
weapon:		rb 1
armor:		rb 1
yendor:		rb 1
level:		rb 1
hp:		rb 2
lfsr:		rb 2
conn:		rb 2
box_w:		rb 1
box_h:		rb 1
hero:		rb 2
debounce:	rb 1
monster:	rb 1
monster_hp:	rb 2
attack:		rb 1
first:		rb 1
temp:		rb 40

