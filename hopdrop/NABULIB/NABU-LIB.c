// ****************************************************************************************
// NABU-LIB C Library
// DJ Sures (c) 2024
// https://nabu.ca
// https://github.com/DJSures/NABU-LIB
//
// Read the NABU-LIB.h file for details of each function in this file.
//
// PERFORMANCE NOTES
// -----------------
// - Almost every function is now hand written Z80 assembly (__naked). The C version of
//   each function is kept in the comments above it so the logic is still easy to follow.
// - Functions with one argument use __z88dk_fastcall (argument in L / HL / DEHL).
// - Functions with two or more arguments use __z88dk_callee: the function pops its own
//   arguments straight into registers. That is smaller and faster than reading them
//   from a stack frame, and the caller no longer needs cleanup code after every call,
//   which also makes every program that calls the library smaller.
// - The tiny helpers (ayWrite, ayRead, vdp_setRegister, the interrupt enable/disable,
//   etc) are macros in NABU-LIB.h, so they compile to a couple of IN/OUT instructions at
//   the call site and no unused callable copy is linked into every program.
//   vdp_setWriteAddress() stays an inline function because it uses its argument twice.
// - Bulk VRAM transfers go through the nl_outBlock / nl_fill loops (also reachable from C
//   as _vdp_outBlock / _vdp_fill). Every write to the VDP data port is at least 29
//   T-states after the previous VRAM access, which is the TMS9918A minimum access time
//   (about 8us in the graphics modes) at 3.58MHz.
// - Row offsets (row * columns) come from the _vdpRowOffset table that vdp_init() fills, so
//   cursor math never needs a software multiply.
//
// REGISTER RULES FOR THE ASSEMBLY
// -------------------------------
// - IX is the SDCC frame pointer and is never touched (or is saved and restored).
// - IY, AF, BC, DE, HL are scratch registers for SDCC in z88dk, so any function may use them.
// - The shadow registers (EXX, EX AF,AF') are not used, because a CP/M BIOS interrupt
//   handler is allowed to use them.
// - Internal helpers start with nl_ and are documented with their register contract.
//
// NOTE ON (void) PARAMETERS
// The __naked assembly functions read their arguments directly from the stack/registers.
// SDCC cannot see inside __asm blocks, so it would report every parameter as unused
// (warning 85). Each one starts with "(void)param;" which marks the parameter as used and
// generates no code. (#pragma disable_warning is not used because in this SDCC version it
// stays off for the rest of the file, which would also hide real warnings in the program
// that includes NABU-LIB.)
//
// **********************************************************************************************


// **************************************************************************
// System
// ------
// **************************************************************************

void initNABULib(void) __naked {

  // // Turn off the rom
  // IO_CONTROL = CONTROL_ROMSEL | CONTROL_VDOBUF;
  //
  // NABU_DisableInterrupts();
  //
  // _randomSeed = R register;
  // srand(_randomSeed);
  //
  // IM 2 with the vector table at INTERUPT_VECTOR_MAP_ADDRESS, then enable the
  // interrupts NABU-LIB uses in the AY port A interrupt mask.
  //
  // if homebrew, there are no existing interupts, so we start with a nice clean 0
  // if cpm, we get the previous interrupt settings that the BIOS set because we only override
  // what we want to use in NABULib.
  //
  // NABU_EnableInterrupts();
  //
  // initNABULIBAudio();
  __asm

      ; Turn off the rom
      ld a, CONTROL_ROMSEL | CONTROL_VDOBUF
      out (_IO_CONTROL), a

      di

      ; seed the random number generator from the refresh register
      ld a, r
      ld (__randomSeed), a
      ld l, a
      ld h, 0
      call _srand

      im 2

      ld a, INTERUPT_VECTOR_MAP_MSB
      ld i, a

    #ifndef DISABLE_HCCA_RX_INT
      ; HCCA Receive
      ld hl, _isrHCCARX
      ld (INTERUPT_VECTOR_MAP_ADDRESS), hl
    #endif

      ; // HCCA Send
      ; ld hl, _isrHCCATX
      ; ld (INTERUPT_VECTOR_MAP_ADDRESS + 2), hl

    #ifndef DISABLE_KEYBOARD_INT
      ; HCCA Keyboard
      ld hl, _isrKeyboard
      ld (INTERUPT_VECTOR_MAP_ADDRESS + 4), hl
    #endif

      ; // Video Frame Sync (see vdp_addISR)
      ; ld hl, _isrVideoSync
      ; ld (INTERUPT_VECTOR_MAP_ADDRESS + 6), hl

      ; // Option cards 0 - 3 would be at +8, +10, +12, +14

    #if BIN_TYPE == BIN_CPM
      ; keep the interrupts the CP/M BIOS already enabled
      ld a, IOPORTA
      out (_IO_AYLATCH), a
      in a, (_IO_AYDATA)
    #else
      ; a homebrew would not have existing interupts, so start with a nice clean 0
      xor a
    #endif

    #ifndef DISABLE_HCCA_RX_INT
      or INT_MASK_HCCARX
    #endif

    #ifndef DISABLE_KEYBOARD_INT
      or INT_MASK_KEYBOARD
    #endif

      ld l, a
      ld a, IOPORTA
      out (_IO_AYLATCH), a
      ld a, l
      out (_IO_AYDATA), a

      ei

      jp _initNABULIBAudio

  __endasm;
}

void initNABULIBAudio(void) __naked {

  // ayWrite(6, 0b00000000);   Noise envelope
  // ayWrite(8, 0b00000000);   Turn off all channels
  // ayWrite(9, 0b00000000);
  // ayWrite(10, 0b00000000);
  // ayWrite(7, 0b01111000);   Enable only the Tone generators on A B C
  // ayWrite(11, 0);
  // ayWrite(12, 0);
  // ayWrite(13, 0);
  __asm

      ld hl, nl_audioInitTable
      ld b, 16

    nl_audioInitLoop:

      ; register, then value. outi decrements b, so two per pair
      ld c, _IO_AYLATCH
      outi
      dec c
      outi
      jr nz, nl_audioInitLoop

      ret

    nl_audioInitTable:

      defb 6, 0x00
      defb 8, 0x00
      defb 9, 0x00
      defb 10, 0x00
      defb 7, 0x78
      defb 11, 0x00
      defb 12, 0x00
      defb 13, 0x00

  __endasm;
}

// **************************************************************************
// Shared memory helpers (register contracts in the comments)
// **************************************************************************
void _nabulib_memCore(void) __naked {

  __asm

    ; ------------------------------------------------------------------------
    ; nl_memset: fill bc bytes at hl with a. bc may be 0.
    ;   in:  hl = destination, bc = count, a = value
    ;   out: a kept. bc, de, hl changed
    ; ------------------------------------------------------------------------
    nl_memset:

      ld e, a
      ld a, b
      or c
      ld a, e
      ret z

      ld (hl), a
      dec bc
      ld a, b
      or c
      ld a, e
      ret z

      ld d, h
      ld e, l
      inc de
      ldir

      ret

  __endasm;
}

// **************************************************************************
// Interrupts
// -----
// **************************************************************************

#ifndef DISABLE_HCCA_RX_INT
  void isrHCCARX(void) __naked {

    // review LIS and only bc, hl, a registers are used for this function
    // (now pure assembly so only the registers actually used, AF and HL, are saved)
    //
    // _rxBuffer[_rxBufferWritePos] = IO_HCCA;
    // _rxBufferWritePos++;
    //
    // The write position is bumped before the byte is stored. That is safe because
    // the main program cannot run while this ISR is executing.
    __asm

      push af
      push hl

      ld hl, __rxBufferWritePos
      ld a, (hl)
      inc (hl)

      ld hl, __rxBuffer
      add a, l
      ld l, a
      adc a, h
      sub l
      ld h, a

      in a, (_IO_HCCA)
      ld (hl), a

      pop hl
      pop af

      ei
      reti

    __endasm;
  }
#endif

#ifndef DISABLE_KEYBOARD_INT
  void isrKeyboard(void) __naked {

    // Pure assembly version of the original C logic below. Writing it in assembly
    // means we know exactly which registers are touched (AF, DE, HL) instead of
    // hoping the C code generator never uses one we did not save.
    //
    // inKey = IO_KEYBOARD
    //
    // if (inKey >= 0x80 && inKey <= 0x83)
    //   _lastKeyboardIntVal = inKey;                   joystick header byte
    // else if (inKey < 0x90 || inKey > 0x95)
    //   if (_lastKeyboardIntVal is 0x80..0x83)
    //     _joyStatus[_lastKeyboardIntVal - 0x80] = inKey; _lastKeyboardIntVal = 0;
    //   else
    //     _kbdBuffer[_kbdBufferWritePos++] = inKey;
    __asm

      push af
      push de
      push hl

      in a, (_IO_KEYBOARD)
      ld e, a

      ; 0x80 - 0x83 is a joystick header, remember which joystick is next
      sub 0x80
      cp 4
      jr nc, isrKbd_notJoyHeader

      ld a, e
      ld (__lastKeyboardIntVal), a
      jr isrKbd_done

    isrKbd_notJoyHeader:

      ; 0x90 - 0x95 are keyboard status codes which are ignored
      ld a, e
      sub 0x90
      cp 6
      jr c, isrKbd_done

      ; was the previous byte a joystick header?
      ld a, (__lastKeyboardIntVal)
      sub 0x80
      cp 4
      jr nc, isrKbd_key

      ; _joyStatus[a] = inKey
      ld hl, __joyStatus
      add a, l
      ld l, a
      adc a, h
      sub l
      ld h, a
      ld (hl), e

      xor a
      ld (__lastKeyboardIntVal), a
      jr isrKbd_done

    isrKbd_key:

      ; _kbdBuffer[_kbdBufferWritePos++] = inKey
      ld hl, __kbdBufferWritePos
      ld a, (hl)
      inc (hl)

      ld hl, __kbdBuffer
      add a, l
      ld l, a
      adc a, h
      sub l
      ld h, a
      ld (hl), e

    isrKbd_done:

      pop hl
      pop de
      pop af

      ei
      reti

    __endasm;
  }
#endif

#if BIN_TYPE == BIN_CPM

#endif


// **************************************************************************
// Keyboard
// --------
// **************************************************************************

#ifndef DISABLE_KEYBOARD_INT

  uint8_t isKeyPressed(void) __naked {

    // return (_kbdBufferWritePos != _kbdBufferReadPos);
    __asm

      ld hl, __kbdBufferReadPos
      ld a, (__kbdBufferWritePos)
      sub (hl)
      ld l, a
      ret z

      ld l, 1
      ret

    __endasm;
  }

  uint8_t getChar(void) __naked {

    // Blocks until a key is waiting. While waiting, the cursor character blinks at the
    // cursor position (unless DISABLE_CURSOR, DISABLE_VDP or CURSOR_CHAR is 0).
    //
    // uint16_t cursorCnt = 0;
    //
    // while (_kbdBufferWritePos == _kbdBufferReadPos)
    //   if (CURSOR_CHAR != 0) {
    //
    //     if (cursorCnt == 1)
    //       vdp_writeCharAtLocation(vdp_cursor.x, vdp_cursor.y, CURSOR_CHAR);
    //     else if (cursorCnt == 15000)
    //       vdp_writeCharAtLocation(vdp_cursor.x, vdp_cursor.y, ' ');
    //     else if (cursorCnt > 30000)
    //       cursorCnt = 0;
    //
    //     cursorCnt++;
    //   }
    //
    // if (CURSOR_CHAR != 0)
    //   vdp_writeCharAtLocation(vdp_cursor.x, vdp_cursor.y, ' ');
    //
    // uint8_t key = _kbdBuffer[_kbdBufferReadPos];
    //
    // _kbdBufferReadPos++;
    //
    // return key;
    //
    // The wait loop is padded to the same ~155 T-states per pass as the old C loop, so
    // the cursor still blinks at the same rate (about 1.3 seconds per blink).
    __asm

    #if defined(DISABLE_CURSOR) || defined(DISABLE_VDP) || (CURSOR_CHAR == 0)

      ld hl, __kbdBufferReadPos

    nl_gcWait:

      ld a, (__kbdBufferWritePos)
      cp (hl)
      jr z, nl_gcWait

      jr nl_kbdPop

    #else

      ld bc, 0                ; cursorCnt

    nl_gcLoop:

      ld hl, __kbdBufferReadPos
      ld a, (__kbdBufferWritePos)
      cp (hl)
      jr nz, nl_gcKey

      ; cursorCnt == 1 ?
      ld a, c
      dec a
      or b
      jr z, nl_gcShow

      ; cursorCnt == 15000 (0x3a98) ?
      ld a, c
      cp 0x98
      jr nz, nl_gcWrap
      ld a, b
      cp 0x3a
      jr z, nl_gcHide

    nl_gcWrap:

      ; cursorCnt > 30000 ?
      ld hl, 30000
      or a
      sbc hl, bc
      jr nc, nl_gcNext
      ld bc, 0

    nl_gcNext:

      inc bc

      ; padding so one pass takes as long as the old C loop (152 T-states)
      ld a, (hl)
      ld a, (hl)

      jr nl_gcLoop

    nl_gcShow:

      ld a, CURSOR_CHAR
      jr nl_gcPut

    nl_gcHide:

      ld a, ' '

    nl_gcPut:

      push bc
      ld de, (_vdp_cursor)
      call nl_writeCharAt
      pop bc
      jr nl_gcNext

    nl_gcKey:

      ; erase the cursor
      ld de, (_vdp_cursor)
      ld a, ' '
      call nl_writeCharAt

    #endif

      jr nl_kbdPop

    __endasm;
  }

  uint8_t getKey(void) __naked {

    // Emulates CP/M BDOS call #6 (direct console input), which returns 0 if no
    // char is waiting. Reads the buffer directly instead of calling getChar()
    // because getChar() also erases the cursor character, which would overwrite
    // whatever is on screen at the cursor position every time a key is polled.
    //
    // if (_kbdBufferWritePos == _kbdBufferReadPos)
    //   return 0;
    //
    // uint8_t key = _kbdBuffer[_kbdBufferReadPos];
    //
    // _kbdBufferReadPos++;
    //
    // return key;
    __asm

      ld hl, __kbdBufferReadPos
      ld a, (__kbdBufferWritePos)
      cp (hl)
      ld l, 0
      ret z

    ; ------------------------------------------------------------------------
    ; nl_kbdPop: remove the next key from the buffer (one must be waiting)
    ;   out: l = a = key. bc and de kept
    ; ------------------------------------------------------------------------
    nl_kbdPop:

      ld a, (__kbdBufferReadPos)
      ld hl, __kbdBuffer
      add a, l
      ld l, a
      adc a, h
      sub l
      ld h, a
      ld a, (hl)

      ld hl, __kbdBufferReadPos
      inc (hl)

      ld l, a
      ret

    __endasm;
  }

  // getJoyStatus() is a macro in NABU-LIB.h

  #ifndef DISABLE_VDP
  #endif

#endif


#ifndef DISABLE_HCCA_RX_INT

  uint8_t hcca_readByte(void) __naked {

    // while (_rxBufferWritePos == _rxBufferReadPos);
    //
    // uint8_t ret = _rxBuffer[_rxBufferReadPos];
    //
    // _rxBufferReadPos++;
    //
    // return ret;
    __asm

    ; ------------------------------------------------------------------------
    ; nl_rxPop: wait for and remove the next received byte
    ;   out: l = a = byte. bc and de kept
    ; ------------------------------------------------------------------------
    nl_rxPop:

      ld hl, __rxBufferReadPos

    nl_rxWait:

      ld a, (__rxBufferWritePos)
      cp (hl)
      jr z, nl_rxWait

      ld a, (hl)
      ld hl, __rxBuffer
      add a, l
      ld l, a
      adc a, h
      sub l
      ld h, a
      ld a, (hl)

      ld hl, __rxBufferReadPos
      inc (hl)

      ld l, a
      ret

    __endasm;
  }

  uint16_t hcca_readUInt16(void) __naked {

    // The low byte is read first (the wire format is LSB first).
    //
    // hcca_readInt16() is the same function (an alias label), the bits are identical.
    __asm

    _hcca_readInt16:

      call nl_rxPop
      ld c, a
      call nl_rxPop
      ld h, a
      ld l, c
      ret

    __endasm;
  }

  uint32_t hcca_readUInt32(void) __naked {

    // The Z80 is little-endian like the wire format, so the 4 bytes are read
    // LSB first into L, H, E, D.
    //
    // hcca_readInt32() is the same function (an alias label), the bits are identical.
    __asm

    _hcca_readInt32:

      call _hcca_readUInt16
      push hl
      call _hcca_readUInt16
      ex de, hl
      pop hl
      ret

    __endasm;
  }

  void hcca_readBytes(uint16_t offset, uint16_t bufferLen, uint8_t *buffer) __z88dk_callee __naked {

    // see NOTE ON (void) PARAMETERS
    (void)offset;
    (void)bufferLen;
    (void)buffer;

    // uint8_t *start = buffer + offset;
    // uint8_t *end   = start + bufferLen;
    //
    // // while (not do/while) so a bufferLen of 0 reads nothing. The do/while version
    // // read 65536 bytes for a length of 0 and hung or overwrote memory, which
    // // happened whenever the Internet Adapter replied with an empty string.
    // while (start != end) {
    //
    //   while (_rxBufferWritePos == _rxBufferReadPos);
    //
    //   *start = _rxBuffer[_rxBufferReadPos];
    //
    //   start++;
    //
    //   _rxBufferReadPos++;
    // }
    __asm

      pop hl
      pop bc                  ; bc = offset
      pop de                  ; de = bufferLen
      ex (sp), hl             ; hl = buffer (return address back on the stack)
      add hl, bc
      ex de, hl               ; de = destination
      ld b, h
      ld c, l                 ; bc = count

    nl_rbLoop:

      ld a, b
      or c
      ret z

      call nl_rxPop
      ld (de), a
      inc de
      dec bc
      jr nl_rbLoop

    __endasm;
  }


  // **************************************************************************
  // HCCA Transmit
  // -------------
  // **************************************************************************

  void hcca_writeByte(uint8_t c) __z88dk_fastcall __naked {

    // see NOTE ON (void) PARAMETERS
    (void)c;

    // NABU_DisableInterrupts();
    //
    // uint8_t origIntMask = ayRead(IOPORTA);
    //
    // ayWrite(IOPORTA, INT_MASK_HCCATX);
    //
    // IO_AYLATCH = IOPORTB;
    // while (IO_AYDATA & 0x04);
    //
    // IO_HCCA = c;
    //
    // ayWrite(IOPORTA, origIntMask);
    //
    // NABU_EnableInterrupts();
    //
    // Only a, f and h are changed, the multi-byte writers depend on bc and de being kept.
    __asm

      di

      ld a, IOPORTA
      out (_IO_AYLATCH), a
      in a, (_IO_AYDATA)
      ld h, a                 ; h = original interrupt mask

      ld a, INT_MASK_HCCATX
      out (_IO_AYDATA), a

      ld a, IOPORTB
      out (_IO_AYLATCH), a

    nl_hwWait:

      in a, (_IO_AYDATA)
      and 0x04
      jr nz, nl_hwWait

      ld a, l
      out (_IO_HCCA), a

      ld a, IOPORTA
      out (_IO_AYLATCH), a
      ld a, h
      out (_IO_AYDATA), a

      ei
      ret

    __endasm;
  }

  // The multi-byte writers send the value LSB first, which matches the wire format.
  // hcca_writeInt32() and hcca_writeInt16() are alias labels of the unsigned versions,
  // the bits sent are identical.
  void hcca_writeUInt32(uint32_t val) __z88dk_fastcall __naked {

    // see NOTE ON (void) PARAMETERS
    (void)val;

    __asm

    _hcca_writeInt32:

      call _hcca_writeUInt16  ; low word (hl), de is kept
      ex de, hl

    _hcca_writeInt16:
    _hcca_writeUInt16:

      push hl
      call _hcca_writeByte
      pop hl
      ld l, h
      jp _hcca_writeByte

    __endasm;
  }

  void hcca_writeBytes(uint16_t offset, uint16_t length, uint8_t *bytes) __z88dk_callee __naked {

    // see NOTE ON (void) PARAMETERS
    (void)offset;
    (void)length;
    (void)bytes;

    // uint8_t *start = bytes + offset;
    // uint8_t *end   = start + length;
    //
    // while (start != end) {
    //
    //   hcca_writeByte(*start);
    //
    //   start++;
    // }
    __asm

      pop hl
      pop bc                  ; bc = offset
      pop de                  ; de = length
      ex (sp), hl             ; hl = bytes (return address back on the stack)
      add hl, bc              ; hl = source

    nl_hbLoop:

      ld a, d
      or e
      ret z

      push hl
      ld l, (hl)
      call _hcca_writeByte
      pop hl

      inc hl
      dec de
      jr nl_hbLoop

    __endasm;
  }
#endif


// **************************************************************************
// VDP
// ---
// (vdp_setRegister, vdp_waitVDPReadyInt, vdp_setBackDropColor and
//  vdp_getCharAtLocationBuf are macros in NABU-LIB.h)
// **************************************************************************

#ifndef DISABLE_VDP

  // Inline: compiles to two OUT instructions at every call site.
  // (A function instead of a macro because the address is used twice.)
  inline void vdp_setWriteAddress(uint16_t address) {

    IO_VDPLATCH = address;

    IO_VDPLATCH = (address >> 8) | 0x40;
  }

  // **************************************************************************
  // Send len bytes from src to VRAM at the current VDP write address.
  // 30 T-states per byte, which is within the TMS9918A access window in every mode.
  // len may be 0. Callable from C with the normal (caller cleans up) convention.
  // **************************************************************************
  void _vdp_outBlock(uint8_t *src, uint16_t len) __naked {

    // see NOTE ON (void) PARAMETERS
    (void)src;
    (void)len;

    __asm

      pop bc
      pop hl
      pop de
      push de
      push hl
      push bc

    ; ------------------------------------------------------------------------
    ; nl_outBlock: stream bytes to VRAM at the current write address
    ;   in:  hl = source, de = count (0 is allowed)
    ;   out: a, bc, de, hl changed
    ; ------------------------------------------------------------------------
    nl_outBlock:

      ld a, d
      or e
      ret z

      ; b = inner count (0 means 256), d = outer passes
      ld b, e
      dec de
      inc d
      ld c, _IO_VDPDATA

    nl_obLoop:

      outi
      nop
      jp nz, nl_obLoop

      dec d
      jp nz, nl_obLoop

      ret

    __endasm;
  }

  // **************************************************************************
  // Write value to VRAM len times at the current VDP write address.
  // 32 T-states per byte. len may be 0. Callable from C with the normal convention.
  // **************************************************************************
  void _vdp_fill(uint16_t len, uint8_t value) __naked {

    // see NOTE ON (void) PARAMETERS
    (void)len;
    (void)value;

    __asm

      pop bc
      pop de
      pop hl
      push hl
      push de
      push bc
      ld a, l

    ; ------------------------------------------------------------------------
    ; nl_fill: write the same value to VRAM at the current write address
    ;   in:  de = count (0 is allowed), a = value
    ;   out: a and hl kept. bc, de changed
    ; ------------------------------------------------------------------------
    nl_fill:

      ld c, a
      ld a, d
      or e
      ld a, c
      ret z

      ld b, e
      dec de
      inc d
      ld c, _IO_VDPDATA

    nl_flLoop:

      out (c), a
      cp 0                    ; padding to 32 T-states per byte
      djnz nl_flLoop

      dec d
      jr nz, nl_flLoop

      ret

    __endasm;
  }

  // **************************************************************************
  // Shared VDP helpers (register contracts in the comments)
  // **************************************************************************
  void _nabulib_vdpCore(void) __naked {

    __asm

    ; ------------------------------------------------------------------------
    ; nl_wrAddr: set the VRAM write address
    ;   in:  hl = VRAM address
    ;   out: a changed. everything else kept
    ; ------------------------------------------------------------------------
    nl_wrAddr:

      ld a, l
      out (_IO_VDPLATCH), a
      ld a, h
      or 0x40
      out (_IO_VDPLATCH), a
      ret

    ; ------------------------------------------------------------------------
    ; nl_outA: write a to VRAM. The call/return keeps back to back writes
    ; far enough apart for the VDP.
    ; ------------------------------------------------------------------------
    nl_outA:

      out (_IO_VDPDATA), a
      ret

    ; ------------------------------------------------------------------------
    ; nl_vregInc: write a to VDP register c, then c++
    ;   out: a changed, c + 1
    ; ------------------------------------------------------------------------
    nl_vregInc:

      out (_IO_VDPLATCH), a
      ld a, c
      or 0x80
      out (_IO_VDPLATCH), a
      inc c
      ret

    ; ------------------------------------------------------------------------
    ; nl_rowOff: hl = _vdpRowOffset[a]
    ;   out: a, de changed. bc kept
    ; ------------------------------------------------------------------------
    nl_rowOff:

      ld l, a
      ld h, 0
      add hl, hl
      ld de, __vdpRowOffset
      add hl, de
      ld a, (hl)
      inc hl
      ld h, (hl)
      ld l, a
      ret

    ; ------------------------------------------------------------------------
    ; nl_nameOff: hl = _vdpRowOffset[y] + x (name table / text buffer offset)
    ;   in:  e = x, d = y
    ;   out: a, de changed. bc kept
    ; ------------------------------------------------------------------------
    nl_nameOff:

      ld a, e
      ld l, d
      ld h, 0
      add hl, hl
      ld de, __vdpRowOffset
      add hl, de
      ld e, (hl)
      inc hl
      ld d, (hl)
      ld l, a
      ld h, 0
      add hl, de
      ret

    ; ------------------------------------------------------------------------
    ; nl_writeCharAt: put a character in the text buffer and on screen at x, y
    ;   in:  e = x, d = y, a = character
    ;   out: c = character. a, de, hl changed. b kept
    ; ------------------------------------------------------------------------
    nl_writeCharAt:

      ld c, a
      call nl_nameOff
      ex de, hl               ; de = offset

      ld hl, (__vdpPatternNameTableAddr)
      add hl, de
      call nl_wrAddr
      ld a, c
      out (_IO_VDPDATA), a

      ld hl, __vdp_textBuffer
      add hl, de
      ld (hl), c
      ret

    ; ------------------------------------------------------------------------
    ; nl_spriteAddr: hl = sprite attribute table address of sprite a
    ;   out: bc, de kept
    ; ------------------------------------------------------------------------
    nl_spriteAddr:

      push de
      ld l, a
      ld h, 0
      add hl, hl
      add hl, hl
      ld de, (__vdpSpriteAttributeTableAddr)
      add hl, de
      pop de
      ret

    ; ------------------------------------------------------------------------
    ; nl_loadThirds: write a block to VRAM at hl. When the pattern/color tables are
    ; split in thirds (G2 bitmap mode) the same block is also written at +2048 and +4096.
    ; datasheet 2-20 : screen is split into 3 and the tables are repeated 3 times
    ;   in:  hl = VRAM address, de = source, bc = length
    ; ------------------------------------------------------------------------
    nl_loadThirds:

      ld a, (__vdpSplitThirds)
      or a
      ld a, 1
      jr z, nl_ltLoop
      ld a, 3

    nl_ltLoop:

      push af
      push hl
      push de
      push bc

      call nl_wrAddr
      ex de, hl               ; hl = source
      ld d, b
      ld e, c                 ; de = length
      call nl_outBlock

      pop bc
      pop de
      pop hl

      ld a, h
      add a, 8                ; + 2048
      ld h, a

      pop af
      dec a
      jr nz, nl_ltLoop

      ret

    ; ------------------------------------------------------------------------
    ; nl_handleEOL: called when the cursor advances past the right margin.
    ; Does one of three things:
    ;
    ;  1) Soft wrap to the next row. The TMS9918A auto-increments its internal
    ;     write-address counter as bytes are pushed to IO_VDPDATA, and the name
    ;     table is row-major contiguous in VRAM, so the chip is already pointing
    ;     at the next rows first byte for free - no setWriteAddress needed.
    ;
    ;  2) Bottom-of-region with autoscroll on: scroll the region up, clamp y to
    ;     the bottom row, and re-set the VDP write address (scrollTextUp leaves
    ;     the address one byte past the region).
    ;
    ;  3) Bottom-of-screen with autoscroll off: clamp y to _vdpCursorMaxY and
    ;     re-set the VDP write address.
    ;
    ;   out: hl = pointer to the new position in the text buffer, so the caller
    ;        can keep its mirror pointer and the VDP internal counter in sync.
    ;        Everything but ix, iy changed.
    ; ------------------------------------------------------------------------
    nl_handleEOL:

      ld hl, _vdp_cursor
      ld (hl), 0              ; x = 0
      inc hl
      inc (hl)                ; y++

      ld a, (__autoScroll)
      or a
      jr z, nl_eolNoScroll

      ld a, (__autoScrollBottomRow)
      cp (hl)
      jr nc, nl_eolNoScroll

      ; y passed the bottom of the scroll region
      ld (hl), a
      ld d, a
      ld a, (__autoScrollTopRow)
      ld e, a
      call nl_scrollUp

      ld a, (_vdp_cursor + 1)
      jr nl_eolSetAddr

    nl_eolNoScroll:

      ld a, (hl)
      cp 24
      jr c, nl_eolSoft

      ld a, 23
      ld (hl), a

    nl_eolSetAddr:

      call nl_rowOff
      push hl
      ld de, (__vdpPatternNameTableAddr)
      add hl, de
      call nl_wrAddr
      pop hl
      ld de, __vdp_textBuffer
      add hl, de
      ret

    nl_eolSoft:

      ; Soft wrap. VDP auto-increment already advanced into the next row.
      call nl_rowOff
      ld de, __vdp_textBuffer
      add hl, de
      ret

    ; ------------------------------------------------------------------------
    ; nl_printSetup: shared by vdp_print and vdp_printPart.
    ; Sets the VDP write address to the cursor position.
    ;   out: de = pointer into _vdp_textBuffer at the cursor
    ;        c  = vdp_cursor.x
    ;        b  = _vdpCursorMaxX
    ;   hl is kept
    ; ------------------------------------------------------------------------
    nl_printSetup:

      push hl

      ; de = _vdpRowOffset[vdp_cursor.y] + vdp_cursor.x
      ld de, (_vdp_cursor)
      ld c, e
      call nl_nameOff
      ex de, hl

      ; vdp_setWriteAddress(_vdpPatternNameTableAddr + name offset)
      ld hl, (__vdpPatternNameTableAddr)
      add hl, de
      call nl_wrAddr

      ; de = _vdp_textBuffer + name offset
      ld hl, __vdp_textBuffer
      add hl, de
      ex de, hl

      ld a, (__vdpCursorMaxX)
      ld b, a

      pop hl
      ret

    ; ------------------------------------------------------------------------
    ; nl_spriteOff: the 4 attribute bytes of a disabled sprite
    ;   y = 192, x = 0, pattern name #0 (should always be empty so no false
    ;   collisions), early clock to hide behind the border
    ; ------------------------------------------------------------------------
    nl_spriteOff:

      defb 192, 0, 0, 0x80

    __endasm;
  }

  void vdp_addISR(void (*isr)(void)) __z88dk_fastcall __naked {

    // see NOTE ON (void) PARAMETERS
    (void)isr;

    // NABU_DisableInterrupts();
    //
    // _vdp_ISR = isr;
    //
    // // Video Frame Sync
    // INTERUPT_VECTOR_MAP_ADDRESS + 6 = isr
    //
    // ayWrite(IOPORTA, ayRead(IOPORTA) | INT_MASK_VDP);
    //
    // NABU_EnableInterrupts();
    //
    // _vdpInterruptEnabled = true;
    // vdp_setRegister(1, _vdpReg1Val | 0b00100000 );
    __asm

      di

      ld (__vdp_ISR), hl

      ; Video Frame Sync
      ld (INTERUPT_VECTOR_MAP_ADDRESS + 6), hl

      ld a, IOPORTA
      out (_IO_AYLATCH), a
      in a, (_IO_AYDATA)
      or INT_MASK_VDP
      out (_IO_AYDATA), a

      ei

      ld a, 1
      ld (__vdpInterruptEnabled), a

      ld a, (__vdpReg1Val)
      or 0x20
      out (_IO_VDPLATCH), a
      ld a, 0x81
      out (_IO_VDPLATCH), a

      ret

    __endasm;
  }

  void vdp_init(uint8_t mode, uint8_t fgColor, uint8_t bgColor, bool big_sprites, bool magnify, bool autoScroll, bool splitThirds) __naked {

    // see NOTE ON (void) PARAMETERS
    (void)mode;
    (void)fgColor;
    (void)bgColor;
    (void)big_sprites;
    (void)magnify;
    (void)autoScroll;
    (void)splitThirds;

    // The per-mode register values come from nl_vdpModeTable (one 8 byte row per mode)
    // instead of a switch statement. This is what each mode sets:
    //
    // VDP_MODE_G2:          R0 = 0x02, R1 = 0xC0 | big<<1 | magnify, R2 = 0x06 (names 0x1800),
    //                       R4 = 0x03 if splitThirds else 0x00 (patterns 0x0000), fgColor = 0, 32 columns
    // VDP_MODE_TEXT40:      R0 = 0x00, R1 = 0xD2, R2 = 0x06 (names 0x1800), R4 = 0x00, 40 columns
    // VDP_MODE_TEXT80:      R0 = 0x04, R1 = 0xD2, R2 = 0x07 (names 0x1000), R4 = 0x00, 80 columns
    // VDP_MODE_MULTICOLOR:  R0 = 0x00, R1 = 0xC8 | big<<1 | magnify, R2 = 0x06 (names 0x1800),
    //                       R4 = 0x00, fgColor = 0, 32 columns
    // VDP_MODE_G1:          G1 has no split tables. Leaving splitThirds on would make the pattern/color
    //                       loaders write copies at +2048/+4096, over the sprite attribute and name tables,
    //                       so splitThirds is forced off.
    //                       R0 = 0x00, R1 = 0xC0 | big<<1 | magnify,
    //                       R2 = 0x05 name table 0x05 * 0x400 = 0x1400
    //                       R3 = 0x80 color table 0x80 * 0x40 = 0x2000
    //                       R4 = 0x01 pattern generator 0x01 * 0x800 = 0x0800
    //                       R5 = 0x20 sprite attributes 0x20 * 0x80 = 0x1000
    //                       R6 = 0x00 sprite patterns 0x00 * 0x800 = 0x0000
    //                       fgColor = 0, 32 columns
    //
    // G1 sets its own color and sprite table locations, so these shared defaults are only
    // applied to the other modes:
    //   R3 = 0xff if splitThirds else 0x9f (colors 0x2000), R5 = 0x36 (sprite attributes 0x1b00),
    //   R6 = 0x07 (sprite patterns 0x3800)
    //
    // Then R7 = (fgColor << 4) | (bgColor & 0x0f), the _vdpRowOffset table is filled
    // (row * columns for every row, and one past the last row, so the text functions can
    // look up a name table offset instead of multiplying), and the screen is cleared.
    __asm

      ; copy the arguments into the library globals, keep fg/bg/magnify in registers
      ld hl, 2
      add hl, sp
      ld a, (hl)
      ld (__vdpMode), a
      inc hl
      ld c, (hl)              ; c = fgColor
      inc hl
      ld b, (hl)              ; b = bgColor
      inc hl
      ld a, (hl)
      ld (__vdpSpriteSizeSelected), a
      inc hl
      ld e, (hl)              ; e = magnify
      inc hl
      ld a, (hl)
      ld (__autoScroll), a
      inc hl
      ld a, (hl)
      ld (__vdpSplitThirds), a

    ; ------------------------------------------------------------------------
    ; nl_vdpInit: shared body of vdp_init and the vdp_initXXXMode functions
    ;   in: c = fgColor, b = bgColor, e = magnify
    ;       _vdpMode, _vdpSplitThirds, _vdpSpriteSizeSelected, _autoScroll already set
    ; ------------------------------------------------------------------------
    nl_vdpInit:

      push bc                 ; fg/bg for R7

      xor a
      ld (__vdpInterruptEnabled), a
      ld h, a
      ld l, a
      ld (_vdp_cursor), hl

      ld a, (__vdpMode)
      dec a
      cp 5
      jp nc, nl_viShared      ; not a known mode: only the shared part is done

      ; hl = nl_vdpModeTable + (mode - 1) * 8
      add a, a
      add a, a
      add a, a
      ld hl, nl_vdpModeTable
      add a, l
      ld l, a
      adc a, h
      sub l
      ld h, a

      ld d, (hl)              ; d = flags
      inc hl

      ; graphic modes ignore the foreground color
      bit 1, d
      jr z, nl_viKeepFg
      pop bc
      ld c, 0
      push bc

    nl_viKeepFg:

      ; R0
      ld c, 0
      ld a, (hl)
      inc hl
      call nl_vregInc

      ; R1 = base | (big_sprites << 1) | magnify in the modes that use sprites
      ld a, (__vdpSpriteSizeSelected)
      add a, a
      or e
      bit 0, d
      jr nz, nl_viSprites
      xor a

    nl_viSprites:

      or (hl)
      inc hl
      ld (__vdpReg1Val), a
      call nl_vregInc

      ; R2
      ld a, (hl)
      inc hl
      call nl_vregInc

      ; R4, or 0x03 in G2 with split thirds
      ld a, (hl)
      inc hl
      bit 2, d
      jr z, nl_viR4
      ld b, a
      ld a, (__vdpSplitThirds)
      or a
      ld a, b
      jr z, nl_viR4
      or 0x03

    nl_viR4:

      ld c, 4
      call nl_vregInc

      ; name table and pattern generator addresses (low bytes are always 0)
      xor a
      ld (__vdpPatternNameTableAddr), a
      ld (__vdpPatternGeneratorTableAddr), a
      ld a, (hl)
      inc hl
      ld (__vdpPatternNameTableAddr + 1), a
      ld a, (hl)
      inc hl
      ld (__vdpPatternGeneratorTableAddr + 1), a

      ; columns
      ld a, (hl)
      ld (__vdpCursorMaxXFull), a
      dec a
      ld (__vdpCursorMaxX), a

      bit 3, d
      jr z, nl_viShared

      ; G1 tables
      xor a
      ld (__vdpSplitThirds), a

      ld c, 3
      ld a, 0x80
      call nl_vregInc

      ld c, 5
      ld a, 0x20
      call nl_vregInc
      xor a
      call nl_vregInc

      ld hl, 0x1000
      ld (__vdpSpriteAttributeTableAddr), hl
      ld h, l
      ld (__vdpSpriteGeneratorTableAddr), hl

      jr nl_viColor

    nl_viShared:

      ld a, (__vdpSplitThirds)
      or a
      ld a, 0x9f
      jr z, nl_viR3
      ld a, 0xff

    nl_viR3:

      ld c, 3
      call nl_vregInc

      ld c, 5
      ld a, 0x36
      call nl_vregInc
      ld a, 0x07
      call nl_vregInc

      ld hl, 0x1b00
      ld (__vdpSpriteAttributeTableAddr), hl
      ld hl, 0x3800
      ld (__vdpSpriteGeneratorTableAddr), hl

    nl_viColor:

      ld hl, 0x2000
      ld (__vdpColorTableAddr), hl

      ; R7 = (fgColor << 4) | (bgColor & 0x0f)
      pop de                  ; e = fg, d = bg
      ld a, e
      add a, a
      add a, a
      add a, a
      add a, a
      ld e, a
      ld a, d
      and 0x0f
      or e
      ld c, 7
      call nl_vregInc

      ; _vdpRowOffset[row] = row * columns, rows 0 to 24
      ld a, (__vdpCursorMaxXFull)
      ld c, a
      ld hl, __vdpRowOffset
      ld de, 0
      ld b, 25

    nl_viRow:

      ld (hl), e
      inc hl
      ld (hl), d
      inc hl
      ld a, e
      add a, c
      ld e, a
      jr nc, nl_viRowNc
      inc d

    nl_viRowNc:

      djnz nl_viRow

      ; _vdpTextBufferSize = _vdpRowOffset[24] (24 rows * columns)
      dec hl
      ld d, (hl)
      dec hl
      ld e, (hl)
      ld (__vdpTextBufferSize), de

      jp _vdp_clearScreen

    ; ------------------------------------------------------------------------
    ; one row per mode (VDP_MODE_G2 = 1 ... VDP_MODE_G1 = 5)
    ;   flags (bit 0 = sprite bits in R1, bit 1 = fgColor forced to 0,
    ;          bit 2 = R4 0x03 when split, bit 3 = G1 tables), R0, R1, R2, R4,
    ;   name table address high byte, pattern generator address high byte, columns
    ; ------------------------------------------------------------------------
    nl_vdpModeTable:

      defb 0x07, 0x02, 0xC0, 0x06, 0x00, 0x18, 0x00, 32   ; G2
      defb 0x03, 0x00, 0xC8, 0x06, 0x00, 0x18, 0x00, 32   ; MULTICOLOR
      defb 0x00, 0x00, 0xD2, 0x06, 0x00, 0x18, 0x00, 40   ; TEXT40
      defb 0x00, 0x04, 0xD2, 0x07, 0x00, 0x10, 0x00, 80   ; TEXT80
      defb 0x0B, 0x00, 0xC0, 0x05, 0x01, 0x14, 0x08, 32   ; G1

    __endasm;
  }

  void vdp_initG2Mode(uint8_t bgColor, bool bigSprites, bool scaleSprites, bool autoScroll, bool splitThirds) __z88dk_callee __naked {

    // see NOTE ON (void) PARAMETERS
    (void)bgColor;
    (void)bigSprites;
    (void)scaleSprites;
    (void)autoScroll;
    (void)splitThirds;

    // vdp_init(VDP_MODE_G2, 0, bgColor, bigSprites, scaleSprites, autoScroll, splitThirds);
    __asm

      ld a, 1                 ; VDP_MODE_G2
      jr nl_initG

    __endasm;
  }

  void vdp_initG1Mode(uint8_t bgColor, bool bigSprites, bool scaleSprites, bool autoScroll, bool splitThirds) __z88dk_callee __naked {

    // see NOTE ON (void) PARAMETERS
    (void)bgColor;
    (void)bigSprites;
    (void)scaleSprites;
    (void)autoScroll;
    (void)splitThirds;

    // splitThirds is ignored by vdp_init() for G1, it is forced to false there
    //
    // vdp_init(VDP_MODE_G1, 0, bgColor, bigSprites, scaleSprites, autoScroll, splitThirds);
    __asm

      ld a, 5                 ; VDP_MODE_G1

    nl_initG:

      ld (__vdpMode), a

      pop hl
      pop bc                  ; c = bgColor, b = bigSprites
      pop de                  ; e = scaleSprites, d = autoScroll
      dec sp
      pop af                  ; a = splitThirds
      push hl

      ld (__vdpSplitThirds), a
      ld a, b
      ld (__vdpSpriteSizeSelected), a
      ld a, d
      ld (__autoScroll), a
      ld b, c                 ; bgColor
      ld c, 0                 ; fgColor
      jp nl_vdpInit

    __endasm;
  }

  void vdp_clearScreen(void) __naked {

    // vdp_setWriteAddress(_vdpPatternNameTableAddr);
    //
    // uint8_t cr = 0x00;
    //
    // if (_vdpMode == VDP_MODE_TEXT40 || _vdpMode == VDP_MODE_TEXT80)
    //   cr = 0x20;
    //
    // // _vdpTextBufferSize is columns * 24 for the current mode. A size of 0 (vdp_init
    // // never called) now does nothing instead of looping through 64KB.
    // _vdp_fill(_vdpTextBufferSize, cr);
    //
    // memset(_vdp_textBuffer, 0x20, _vdpTextBufferSize);
    __asm

      ; text modes (3 and 4) clear with spaces, graphic modes with pattern 0
      ld a, (__vdpMode)
      sub 3
      cp 2
      ld a, 0x20
      jr c, nl_fillScreenA

      xor a
      call nl_fillScreenVRAM
      ld a, 0x20
      jr nl_fillScreenBuf

    __endasm;
  }

  void vdp_fillScreen(uint8_t c) __z88dk_fastcall __naked {

    // see NOTE ON (void) PARAMETERS
    (void)c;

    // vdp_setWriteAddress(_vdpPatternNameTableAddr);
    //
    // _vdp_fill(_vdpTextBufferSize, c);
    //
    // memset(_vdp_textBuffer, c, _vdpTextBufferSize);
    __asm

      ld a, l

    nl_fillScreenA:

      push af
      call nl_fillScreenVRAM
      pop af

    nl_fillScreenBuf:

      ld hl, __vdp_textBuffer
      ld bc, (__vdpTextBufferSize)
      jp nl_memset

    ; nl_fillScreenVRAM: fill the name table with a (a kept)
    nl_fillScreenVRAM:

      ld hl, (__vdpPatternNameTableAddr)
      ld c, a
      call nl_wrAddr
      ld a, c
      ld de, (__vdpTextBufferSize)
      jp nl_fill

    __endasm;
  }

  void vdp_writeCharAtLocation(uint8_t x, uint8_t y, uint8_t c) __z88dk_callee __naked {

    // see NOTE ON (void) PARAMETERS
    (void)x;
    (void)y;
    (void)c;

    // uint16_t name_offset = _VDP_NAME_OFFSET(x, y);
    //
    // _vdp_textBuffer[name_offset] = c;
    //
    // vdp_setWriteAddress(_vdpPatternNameTableAddr + name_offset);
    //
    // IO_VDPDATA = c;
    //
    // vdp_putPattern() does exactly the same, so it is an alias label.
    __asm

    _vdp_putPattern:

      pop hl
      pop de                  ; e = x, d = y
      dec sp
      pop af                  ; a = c
      push hl

      jp nl_writeCharAt

    __endasm;
  }
  void vdp_print(uint8_t *text) __z88dk_fastcall __naked {

    // see NOTE ON (void) PARAMETERS
    (void)text;

    // Fast streaming path: set the VDP write address once, then push bytes
    // and let the chip's auto-incrementing address counter advance through
    // VRAM. The cold path (right-margin wrap) is handled by nl_handleEOL.
    //
    // Everything lives in registers: hl = text, de = text buffer, c = column,
    // b = max column. About 72 T-states per character, well above the VDP
    // minimum access time.
    __asm

      ld a, (hl)
      or a
      ret z

      call nl_printSetup

    vdpPrint_loop:

      ld a, (hl)
      or a
      jr z, vdpPrint_done

      out (_IO_VDPDATA), a
      ld (de), a

      inc de
      inc hl
      inc c

      ; column past the right margin?
      ld a, b
      cp c
      jr nc, vdpPrint_loop

      push hl
      push bc
      call nl_handleEOL
      ex de, hl
      pop bc
      pop hl

      ld c, 0
      jr vdpPrint_loop

    vdpPrint_done:

      ld a, c
      ld (_vdp_cursor), a

      ret

    __endasm;
  }

  void vdp_printPart(uint16_t offset, uint16_t textLength, uint8_t *text) __z88dk_callee __naked {

    // see NOTE ON (void) PARAMETERS
    (void)offset;
    (void)textLength;
    (void)text;

    // Same streaming pattern as vdp_print, terminating on length instead
    // of null. Sharing a helper for the per-character body would cost a
    // function call per byte on Z80 and undo the speedup, so the loop is
    // intentionally inlined twice. The shared cold path lives in
    // nl_handleEOL.
    //
    // Assembly: hl = text, iy = text buffer, de = bytes left, c = column,
    // b = max column. (IY is a scratch register for SDCC in z88dk.)
    __asm

      pop hl
      pop bc                  ; bc = offset
      pop de                  ; de = textLength
      ex (sp), hl             ; hl = text (return address back on the stack)

      ; hl = text + offset, de = textLength
      add hl, bc

      ld a, d
      or e
      ret z

      push de
      call nl_printSetup
      push de
      pop iy
      pop de

    vdpPrintPart_loop:

      ld a, (hl)
      out (_IO_VDPDATA), a
      ld (iy+0), a

      inc iy
      inc hl
      inc c

      ; column past the right margin?
      ld a, b
      cp c
      jr nc, vdpPrintPart_next

      push hl
      push de
      push bc
      call nl_handleEOL
      push hl
      pop iy
      pop bc
      pop de
      pop hl

      ld c, 0

    vdpPrintPart_next:

      dec de
      ld a, d
      or e
      jr nz, vdpPrintPart_loop

      ld a, c
      ld (_vdp_cursor), a

      ret

    __endasm;
  }

  void vdp_scrollTextUp(uint8_t topRow, uint8_t bottomRow) __z88dk_callee __naked {

    // see NOTE ON (void) PARAMETERS
    (void)topRow;
    (void)bottomRow;

    // // Scrolls rows topRow..bottomRow (inclusive) up by one and blanks bottomRow.
    // if (bottomRow > VDP_MAX_Y)
    //   bottomRow = VDP_MAX_Y;
    //
    // // topRow == bottomRow just blanks that row. Before, it looped through all of RAM.
    // if (topRow > bottomRow)
    //   return;
    //
    // uint16_t name_offset = _vdpRowOffset[topRow];
    // uint16_t moveLen     = _vdpRowOffset[bottomRow] - name_offset;
    // uint8_t *to          = _vdp_textBuffer + name_offset;
    //
    // // Move the buffer with LDIR, blank the last row, then stream the whole
    // // region to VRAM in one pass with the block writer.
    // memmove(to, to + _vdpCursorMaxXFull, moveLen);
    //
    // memset(to + moveLen, 0x20, _vdpCursorMaxXFull);
    //
    // vdp_setWriteAddress(_vdpPatternNameTableAddr + name_offset);
    //
    // _vdp_outBlock(to, moveLen + _vdpCursorMaxXFull);
    __asm

      pop hl
      pop de                  ; e = topRow, d = bottomRow
      push hl

    ; internal entry: e = topRow, d = bottomRow
    nl_scrollUp:

      call nl_scrollRange
      ret c

      push de                 ; name offset
      push bc                 ; moveLen

      ld hl, __vdp_textBuffer
      add hl, de
      ex de, hl               ; de = to
      ld a, (__vdpCursorMaxXFull)
      ld l, a
      ld h, 0
      add hl, de              ; hl = to + columns

      ld a, b
      or c
      jr z, nl_suNoMove
      ldir                    ; de ends at to + moveLen

    nl_suNoMove:

      ex de, hl               ; hl = to + moveLen

    ; ------------------------------------------------------------------------
    ; nl_scrollBlank: blank one row at hl, then send the region to VRAM
    ;   stack: name offset, moveLen (top)
    ; ------------------------------------------------------------------------
    nl_scrollBlank:

      ld a, (__vdpCursorMaxXFull)
      ld c, a
      ld b, 0
      ld a, 0x20
      call nl_memset

      pop bc                  ; bc = moveLen
      pop de                  ; de = name offset

      ld hl, (__vdpPatternNameTableAddr)
      add hl, de
      call nl_wrAddr

      ld hl, __vdp_textBuffer
      add hl, de              ; hl = first row of the region

      ld a, (__vdpCursorMaxXFull)
      add a, c
      ld e, a
      ld a, b
      adc a, 0
      ld d, a                 ; de = moveLen + columns
      jp nl_outBlock

    ; ------------------------------------------------------------------------
    ; nl_scrollRange: clamp bottomRow to the last row and find the region
    ;   in:  e = topRow, d = bottomRow
    ;   out: carry set if there is nothing to do (topRow > bottomRow)
    ;        else de = _vdpRowOffset[topRow], bc = _vdpRowOffset[bottomRow] - de
    ; ------------------------------------------------------------------------
    nl_scrollRange:

      ld a, d
      cp 24
      jr c, nl_srBottomOk
      ld d, 23

    nl_srBottomOk:

      ld a, d
      cp e
      ret c

      ld b, d
      ld a, e
      call nl_rowOff
      push hl
      ld a, b
      call nl_rowOff
      pop de
      or a
      sbc hl, de
      ld b, h
      ld c, l
      or a
      ret

    __endasm;
  }
#endif
