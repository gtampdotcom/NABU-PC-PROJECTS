// ****************************************************************************************
// HOP OR DROP -- a rhythm survival party game for the NABU PC
//
// 4 players (humans and CPU robots), one lane each, stacked down the screen, with a lava
// pit behind them. Every hazard is called one beat ahead: a low BOOM and a green up arrow
// mean HOP, a high PING and a red down arrow mean DROP. On the next beat it rolls through
// every lane at once: a spiky ball must be hopped, a bat must be dropped under. A hit
// knocks you one step back towards the lava; the seventh hit knocks you in. The tempo
// keeps rising; the last one standing wins the round, the first to three rounds wins the
// match. The leaderboard (nabu.ca) is the time survived in one round.
//
// Controls: joysticks 1-4: UP or FIRE = hop, DOWN = drop; the arrow keys are joystick 1.
// Menus: any stick or the arrow keys (FIRE / RIGHT / RETURN chooses); ESC goes back.
// Joining: HOP on a stick that isn't playing takes a robot's place, even mid-round.
//
// Online (RetroNET-Multiplayer.h, nabu.ca): the hazards come only from a seed and the
// beat clock (identical on every NABU), and each player's actions are events (hop / drop
// at a time), so a remote player is just another input source. See "Online" below.
//
// Build with z88dk (+nabu) and NABU-LIB: see README.md.
// ****************************************************************************************

#define BIN_TYPE BIN_HOMEBREW
#define FONT_STANDARD
#define DISABLE_CURSOR

#include "NABULIB/NABU-LIB.h"

// z88dk's nabu start-up code always links its text console driver (ANSI emulation, a font,
// a shadow text buffer: several KB). Nothing here prints through stdio, so the build
// redirects fputc_cons to this stub (-pragma-redirect:fputc_cons=_fputc_cons_stub) and the
// driver is left out.
void fputc_cons_stub(void) __naked {
  __asm
  ret
  __endasm;
}
#include "NABULIB/patterns.h"
#include "gfx.h"
#include "notes.h"
#include "song.h"
#define MP_FOCUS_KEEP_KEYBOARD   // joysticks keep working during adapter calls
#include "NABULIB/RetroNET-Multiplayer.h"

// the game id registered at nabu.ca/my/multiplayer
#define MP_GAME_ID    23
// the nabu.ca leaderboard (defined on the game's page): the time survived in a round, in
// tenths of a second, higher is better
#define LB_BOARD      0
// this build's online version: the host puts it in session data byte 0; a NABU that joins
// a session of another version won't play in it (raise it whenever online play changes)
#define GAME_VERSION  1

// ---------------------------------------------------------------------------
// Rules and layout
// ---------------------------------------------------------------------------
#define MAXP          4
#define START_STEPS   7          // hits until you are in the lava (FRONT_COL 9 back to col 3, the lava edge)
#define WINS_NEEDED   3
#define START_BPM     96
#define MAX_BPM       200
#define BPM_STEP      3          // every BPM_EVERY hazards
#define BPM_EVERY     6

// time is in ticks: 256 ticks = one beat
#define JUMP_TICKS    205        // a jump lasts 0.8 beat
#define DUCK_TICKS    179        // a duck lasts 0.7 beat
// when a hop / drop makes you safe, in ticks since it started: a ball misses you while the
// hop is at least 12 high ((ph*(256-ph))>>10, ph = phase of 256); a bat misses you between
// drop phases 38 and 218
#define JUMP_SAFE_FROM  52
#define JUMP_SAFE_TO    154
#define DUCK_SAFE_FROM  27
#define DUCK_SAFE_TO    153
const uint8_t JUMP_H[52] = { 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 4, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 6, 6, 6, 6, 5, 5, 5, 4, 4, 3, 3, 3, 2, 1, 1, 1, 1 };   // drawn jump height by (ticks >> 2)
#define BUFFER_TICKS  51         // a press this close to the end of an action is queued

#define ACT_NONE      0
#define ACT_JUMP      1
#define ACT_DUCK      2

#define HZ_LOW        0          // ball: jump it
#define HZ_HIGH       1          // bat: duck it

// input sources
#define SRC_JOY1      0          // .. SRC_JOY4 = 3 (the arrow keys are joystick 1)
#define NUM_SRC       4
#define SRC_CPU       6
#define SRC_NET       7          // online: a player on another NABU
#define SRC_ANY       8          // online: this NABU's player, any stick or keys

// screen: one lane of 3 tile rows per player; lava in columns 0-2, the player starts at
// column FRONT_COL and is knocked back one column (8 px) per hit
#define LANE_H        3
#define LAVA_COLS     3
#define FRONT_COL     9
#define SINK_COL      1
#define LABEL_COL     12
#define MSG_ROW       19
// the roller moves ROLL_PX px a beat (3/8 px a tick): a step back (8 px) = 64/3 ticks later
#define ROLL_PX       96
#define ROLL_X(e)     (((int16_t)(e) * 3) >> 3)      // e * ROLL_PX / 256
const int16_t STEP_T[START_STEPS + 1] = { 0, 21, 43, 64, 85, 107, 128, 149 };   // k * 8 px * 256 / ROLL_PX
#define STEP_TICKS(k) (STEP_T[k])

const uint8_t PCOLOR[MAXP] = { VDP_LIGHT_RED, VDP_LIGHT_BLUE, VDP_LIGHT_GREEN, VDP_LIGHT_YELLOW };

// tile ids (the same in all three thirds)
#define T_BLANK       0
#define T_BRICK       3
#define T_LAVATOP     20
#define T_LAVA        21
#define T_FLOOR       22
#define T_FLAME       23
#define T_WSKULL      24         // small skull in the brick walls
#define T_LSKULL      25         // small skull floating in the lava
#define T_BIGSKULL    26         // 4 tiles
#define T_ARROWUP     8          // 4 tiles
#define T_ARROWDN     12         // 4 tiles
#define T_PLAYER      128        // 6 tiles per player
#define T_HEAD        160        // 4 tiles per player: their picture above the lanes

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
typedef struct {
  uint8_t  src;          // input source
  uint8_t  num;          // player number 1-4 (for P1 / CPU2 names)
  uint8_t  color;
  uint8_t  pos;          // steps knocked back (START_STEPS = in the lava)
  uint8_t  wins;
  uint8_t  col;          // tile column drawn at, 0xFF = not on screen
  uint8_t  act;          // ACT_*
  uint8_t  queued;       // ACT_* to start when the current action ends
  uint16_t actStart;     // tick (low 16 bits of T) the action started
  uint16_t hurtUntil;
  bool     hurt;
  bool     falling;
  uint16_t fallStart;
  uint8_t  lastKey;      // last drawn look, to redraw only on change
  uint8_t  lastColor;
  // CPU: up to two planned presses
  uint8_t  planAct[2];
  uint16_t planTime[2];
  uint8_t  cpuErr;       // timing error in frames (skill)
  uint16_t survived;     // frames from GO (or joining) until falling in (this round)
  uint16_t joinFrame;    // _playFrames when this player joined the round (0 = from GO)
  // names and online
  char     name[16];     // shown on the lane (up to 15 characters)
  uint8_t  owner;        // online: the session player id whose NABU runs this player
  bool     cpu;          // online: a CPU player (run by its owner)
} Player;

Player   _pl[MAXP];
uint8_t  _numPlayers;
uint8_t  _alive;
uint8_t  _laneTop0;               // tile row of lane 0

typedef struct {
  uint16_t hitBeat;      // the beat it reaches the front position
  uint8_t  type;
  uint8_t  judged;       // bit per player
  bool     active;
} Hazard;

#define MAXHZ 8
Hazard   _hz[MAXHZ];
uint16_t _nextHzBeat;
uint16_t _hzCount;
bool     _spawning;

// beat clock
uint16_t _bpm;
uint16_t _fracInc;
uint16_t _beatNum;
uint16_t _beatFrac;
uint16_t _T16;
uint8_t  _ticksPerFrame;
uint16_t _frame;

uint8_t  _round;
uint16_t _roundStartBeat;
uint8_t  _gameState;
#define GS_COUNTIN  0
#define GS_PLAY     1
#define GS_OVER     2
uint16_t _overBeat;

// input edges per source
uint8_t  _srcJump[NUM_SRC];
uint8_t  _srcDuck[NUM_SRC];
uint8_t  _lastJoy[4];
bool     _escPressed;
bool     _anyKey;
bool     _demo;

// RNG (hazards get their own, so an online game can share the seed)
uint16_t _rng = 0x1234;
#define ALIVE(pl) ((pl)->pos < START_STEPS)
uint16_t _hzRng = 0xACE1;

uint8_t  _nt[768];       // name table copy

// online (RetroNET Multiplayer)
bool     _mpOk;          // the adapter is new enough for multiplayer
char     _myName[16];    // the player name from the adapter's settings ("" = none)
char     _myLabel[16];   // the same, cleaned up for the screen
bool     _online;
bool     _isHost;
uint8_t  _myId;
MpStatus _mst;
MpSessionInfo _si;     // (128 bytes: kept off the stack)
uint8_t  _rx[32];        // received packet
uint8_t  _tx[8];         // packet to send
uint8_t  _rbuf[32];      // the last round packet ('R')
bool     _haveR;         // _rbuf holds a round not started yet
uint8_t  _rxFrom;        // who sent _rx
uint8_t  _sbuf[32];      // a mid-round join snapshot ('S') for this NABU
bool     _haveS;         // _sbuf holds one not played yet
bool     _midJoin;       // playRound: join the round in progress from the snapshot
uint8_t  _midPos[MAXP];  // the snapshot's steps per player
uint16_t _roundSeed;     // the round's hazard seed (for snapshots)
bool     _inRound;       // A / H / E packets for _round are taken
bool     _netEnd;        // the host has ended the round
uint8_t  _netWinner;     // the round's winner (0xFF = nobody)
bool     _netMatchOver;
bool     _winnerSet;     // the round's winner is known (the last one standing plays on)
uint16_t _playFrames;    // frames since GO
bool     _mpReady;       // mp_init done (leaderboard / online)
bool     _mpFailed;      // mp_init failed: don't keep trying
uint8_t  _netRound;
uint32_t _startTime;     // mp_time() at which the round's clock starts

// sound
uint8_t  _volA, _volB, _volC;
bool     _noiseOn;

// ---------------------------------------------------------------------------
// Frame timing: the VDP interrupt counts vsyncs, so the music and the beat clock
// catch up when a frame of game work runs long instead of slowing down.
// ---------------------------------------------------------------------------
volatile uint8_t vsyncCount;
uint8_t  _lastVsync;
uint16_t _lateFrames;            // frames that took more than one vsync (debug)

void frameISR(void) __naked {
  __asm
    push af
    in   a, (_IO_VDPLATCH)
    ld   (_vdpStatusRegVal), a
    ld   a, 1
    ld   (_vdpIsReady), a
    ld   a, (_vsyncCount)
    inc  a
    ld   (_vsyncCount), a
    pop  af
    ei
    reti
  __endasm;
}

// wait for the next vsync; returns how many vsyncs have passed since the last call
// (a frame that runs a little long continues at once instead of waiting a whole
// extra vsync, which vdp_waitVDPReadyInt would do)
uint8_t waitFrame(void) {
  while (vsyncCount == _lastVsync);
  uint8_t n = vsyncCount - _lastVsync;
  _lastVsync += n;
  if (n > 1) _lateFrames++;
  if (n > 6) n = 6;
  if (n == 0) n = 1;
  return n;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
uint16_t rnd(void) {
  _rng ^= _rng << 7;
  _rng ^= _rng >> 9;
  _rng ^= _rng << 8;
  return _rng;
}

uint16_t hzRnd(void) {
  _hzRng ^= _hzRng << 7;
  _hzRng ^= _hzRng >> 9;
  _hzRng ^= _hzRng << 8;
  return _hzRng;
}

// The frame interrupt reads the VDP status register, which resets the VDP's address
// latch: if it lands between the two address bytes, the following data goes to the
// wrong place (the lava animation ended up in the floor tile). So no interrupts between
// the two bytes.
void vaddr(uint16_t a) __z88dk_fastcall __naked {
  (void)a;
  __asm
    di
    ld   a, l
    out  (0xA1), a
    ld   a, h
    or   0x40
    out  (0xA1), a
    ei
    ret
  __endasm;
}

// OUTI + NOP + JP = 30 cycles a byte (8.4 us): still within the TMS9918's 8 us rule
// for writes during the active display, about twice as fast as the C loop
const uint8_t *_vp;
uint8_t _vn;
void voutFast(void) __naked {
  __asm
    ld   hl, (__vp)
    ld   a, (__vn)
    ld   b, a
    ld   c, 0xA0
  voutFastLoop:
    outi
    nop
    jp   nz, voutFastLoop
    ret
  __endasm;
}

void vout(const uint8_t *p, uint16_t n) {
  while (n) {
    uint8_t k = n > 255 ? 255 : (uint8_t)n;
    _vp = p;
    _vn = k;
    voutFast();
    p += k;
    n -= k;
  }
}

void vfill(uint8_t v, uint16_t n) {
  while (n--)
    IO_VDPDATA = v;
}

// a tile pattern + one colour byte for all 8 lines, in all three thirds
void loadTile(uint8_t id, const uint8_t *pat, uint8_t color) {
  for (uint8_t t = 0; t < 3; t++) {
    vaddr(_vdpPatternGeneratorTableAddr + (t * 0x800) + (uint16_t)id * 8);
    vout(pat, 8);
    vaddr(_vdpColorTableAddr + (t * 0x800) + (uint16_t)id * 8);
    vfill(color, 8);
  }
}

void ntPut(uint8_t col, uint8_t row, uint8_t id) {
  uint16_t o = (uint16_t)row * 32 + col;
  _nt[o] = id;
  vaddr(_vdpPatternNameTableAddr + o);
  IO_VDPDATA = id;
}

void ntFlush(void) {
  vaddr(_vdpPatternNameTableAddr);
  vout(_nt, 768);
}

// text into the name table copy (and VRAM when direct)
void printAt(uint8_t col, uint8_t row, const char *s, bool direct) {
  uint16_t o = (uint16_t)row * 32 + col;
  const char *p = s;
  while (*p && col < 32) {
    _nt[o++] = *p++;
    col++;
  }
  if (direct) {
    o = (uint16_t)row * 32 + (col - (p - s));
    vaddr(_vdpPatternNameTableAddr + o);
    vout(&_nt[o], p - s);
  }
}

void clearRow(uint8_t row, bool direct) {
  for (uint8_t c = 0; c < 32; c++)
    _nt[(uint16_t)row * 32 + c] = T_BLANK;
  if (direct) {
    vaddr(_vdpPatternNameTableAddr + (uint16_t)row * 32);
    vout(&_nt[(uint16_t)row * 32], 32);
  }
}

void centerAt(uint8_t row, const char *s, bool direct) {
  uint8_t n = 0;
  while (s[n]) n++;
  clearRow(row, direct);
  printAt((32 - n) / 2, row, s, direct);
}

char *u8str(uint8_t v, char *buf) {
  // up to 3 digits
  char *p = buf;
  if (v >= 100) { *p++ = '0' + v / 100; v %= 100; *p++ = '0' + v / 10; v %= 10; }
  else if (v >= 10) { *p++ = '0' + v / 10; v %= 10; }
  *p++ = '0' + v;
  *p = 0;
  return buf;
}

// ---------------------------------------------------------------------------
// Sound: the music on A (bass + drum noise), B (lead), C (second voice); the hit
// noise borrows C for a moment
// ---------------------------------------------------------------------------
// Music: "Exit the Premises" by Kevin MacLeod (incompetech.com),
// licensed under Creative Commons: By Attribution 4.0 (creativecommons.org/licenses/by/4.0/).
// The whole song, read from the piano roll in the official video (music/roll.py,
// music/mksong.py -> song.h), played a little faster than the original 128 BPM.
#define MUSIC_BPM 150

void tone(uint8_t ch, uint8_t note) {
  uint16_t p = NOTE_PERIOD[note - 24];
  ayWrite(ch * 2, p & 0xFF);
  ayWrite(ch * 2 + 1, p >> 8);
}

uint8_t _drumT;                  // frames of drum noise left on channel A
uint8_t _holdA, _holdB, _holdC;  // sixteenths a note is still held

// tones on, noise on A for a drum, noise instead of tone on C for a hit; bit 6 keeps
// port A an output (the interrupt mask)
void setMixer(void) {
  uint8_t v = 0x78;
  if (_drumT) v &= ~0x08;
  if (_noiseOn) v = (v & ~0x20) | 0x04;
  ayWrite(7, v);
}

void sndInit(void) {
  ayWrite(8, 0);
  ayWrite(9, 0);
  ayWrite(10, 0);
  _volA = _volB = _volC = 0;
  _holdA = _holdB = _holdC = 0;
  _noiseOn = false;
  _drumT = 0;
  setMixer();
}

void sndSilence(void) {
  sndInit();
}

void sndHit(void) {
  ayWrite(6, 20);
  _noiseOn = true;
  setMixer();
  _volC = 15;
  ayWrite(10, _volC);
}

// the music runs on its own clock (sixteenth notes), not the game tempo
typedef struct {
  const uint8_t *p;
  uint8_t wait;
} Stream;

const uint8_t *const SONG_STREAMS[4] = { SONG_A, SONG_B, SONG_C, SONG_D };
Stream   _ms[4];
uint16_t _musFrac;

void musicReset(void) {
  _musFrac = 0;
  for (uint8_t k = 0; k < 4; k++) {
    _ms[k].p = SONG_STREAMS[k];
    _ms[k].wait = _ms[k].p[0];
  }
}

void musicNote(uint8_t k, uint8_t note, uint8_t len) {
  if (!note) return;
  if (k == 0) { tone(0, note); _volA = 13; _holdA = len; ayWrite(8, _volA); }
  else if (k == 1) { tone(1, note); _volB = 13; _holdB = len; ayWrite(9, _volB); }
  else if (k == 2) {
    if (_noiseOn) return;        // the hit noise has channel C for a moment
    tone(2, note); _volC = 10; _holdC = len; ayWrite(10, _volC);
  } else {                       // drum: a short noise burst on the bass channel
    ayWrite(6, note == 56 ? 4 : 14);
    _drumT = 3;
    setMixer();
    if (_volA < 11) { _volA = 11; ayWrite(8, _volA); }
  }
}

void musicStep(void) {
  bool restart = false;
  if (_holdA) _holdA--;
  if (_holdB) _holdB--;
  if (_holdC) _holdC--;
  for (uint8_t k = 0; k < 4; k++) {
    Stream *s = &_ms[k];
    while (s->wait == 0) {
      musicNote(k, s->p[1], s->p[2]);
      s->p += 3;
      if (s->p[0] == 0xFF) { restart = true; break; }
      s->wait = s->p[0];
    }
    if (s->wait) s->wait--;
  }
  if (restart) musicReset();
}

void musicTick(void) {
  uint16_t old = _musFrac;
  _musFrac += (uint16_t)(((uint32_t)MUSIC_BPM << 18) / 3600);   // 4 sixteenths a beat
  if (_musFrac < old) musicStep();
}

// volume: a held note fades slowly, a released one quickly
uint8_t envelope(uint8_t vol, uint8_t hold, uint8_t floor) {
  if (!vol) return 0;
  if (hold) return ((_frame & 7) == 0 && vol > floor) ? vol - 1 : vol;
  return vol > 2 ? vol - 2 : 0;
}

void sndFrame(void) {
  uint8_t v;
  v = envelope(_volA, _holdA, 9);
  if (v != _volA) { _volA = v; ayWrite(8, v); }
  v = envelope(_volB, _holdB, 10);
  if (v != _volB) { _volB = v; ayWrite(9, v); }
  if (_noiseOn) {
    if (_volC) _volC--;
    ayWrite(10, _volC);
    if (!_volC) { _noiseOn = false; setMixer(); }
  } else {
    v = envelope(_volC, _holdC, 7);
    if (v != _volC) { _volC = v; ayWrite(10, v); }
  }
  if (_drumT && !--_drumT) setMixer();
}

// ---------------------------------------------------------------------------
// Beat clock
// ---------------------------------------------------------------------------
void setBpm(uint16_t bpm) {
  _bpm = bpm;
  _fracInc = (uint16_t)(((uint32_t)bpm << 16) / 3600);   // 60 frames a second
  _ticksPerFrame = _fracInc >> 8;
}

void clockReset(void) {
  _beatNum = 0;
  _beatFrac = 0;
  _T16 = 0;
}

// ---------------------------------------------------------------------------
// Graphics setup
// ---------------------------------------------------------------------------
// a tile pattern + 8 colour bytes, in all three thirds
void loadTileC(uint8_t id, const uint8_t *pat, const uint8_t *cols) {
  for (uint8_t t = 0; t < 3; t++) {
    vaddr(_vdpPatternGeneratorTableAddr + (t * 0x800) + (uint16_t)id * 8);
    vout(pat, 8);
    vaddr(_vdpColorTableAddr + (t * 0x800) + (uint16_t)id * 8);
    vout(cols, 8);
  }
}

void loadPat(uint8_t id, const uint8_t *pat) {
  for (uint8_t t = 0; t < 3; t++) {
    vaddr(_vdpPatternGeneratorTableAddr + (t * 0x800) + (uint16_t)id * 8);
    vout(pat, 8);
  }
}

#define FG(c) ((uint8_t)((uint8_t)(c) << 4))     // a foreground colour
#define LAVA_COLOR (FG(VDP_DARK_YELLOW) | VDP_MED_RED)
const uint8_t LAVATOP_COLS[8] = { FG(VDP_MED_RED), FG(VDP_MED_RED), FG(VDP_MED_RED), FG(VDP_MED_RED),
                                  LAVA_COLOR, LAVA_COLOR, LAVA_COLOR, LAVA_COLOR };

// flames: yellow tips, orange middle, red base (one colour per pixel line)
const uint8_t FLAME_COLS[8] = { FG(VDP_LIGHT_YELLOW), FG(VDP_LIGHT_YELLOW), FG(VDP_LIGHT_YELLOW),
                                FG(VDP_DARK_YELLOW), FG(VDP_DARK_YELLOW),
                                FG(VDP_MED_RED), FG(VDP_MED_RED), FG(VDP_MED_RED) };

void loadGraphics(void) {
  // font: ASCII 32-127 in all thirds, white on transparent
  for (uint8_t t = 0; t < 3; t++) {
    vaddr(_vdpPatternGeneratorTableAddr + (t * 0x800) + 32 * 8);
    vout(ASCII, 768);
    vaddr(_vdpColorTableAddr + (t * 0x800) + 32 * 8);
    vfill(0xF0, 768);
    // tile 0 blank
    vaddr(_vdpPatternGeneratorTableAddr + (t * 0x800));
    vfill(0, 8);
    vaddr(_vdpColorTableAddr + (t * 0x800));
    vfill(0, 8);
  }
  loadTile(T_BRICK,  TILE_BRICK,  FG(VDP_BLACK) | VDP_DARK_RED);
  loadTile(T_LAVA,   TILE_LAVA0,  LAVA_COLOR);
  loadTileC(T_LAVATOP, TILE_LAVATOP0, LAVATOP_COLS);
  loadTile(T_FLOOR,  TILE_FLOOR,  FG(VDP_DARK_RED));
  loadTileC(T_FLAME, TILE_FLAME0, FLAME_COLS);
  loadTile(T_WSKULL, TILE_WSKULL, FG(VDP_GRAY) | VDP_DARK_RED);
  loadTile(T_LSKULL, TILE_WSKULL, FG(VDP_WHITE) | VDP_MED_RED);
  for (uint8_t i = 0; i < 4; i++) {
    loadTile(T_ARROWUP + i, TILE_ARROWUP + i * 8, FG(VDP_LIGHT_GREEN));
    loadTile(T_ARROWDN + i, TILE_ARROWDN + i * 8, FG(VDP_MED_RED));
    loadTile(T_BIGSKULL + i, TILE_BIGSKULL + i * 8, FG(VDP_WHITE));
  }
  // sprites: ball 0,4  bat 8,12
  vaddr(_vdpSpriteGeneratorTableAddr);
  vout(SPRITES, sizeof(SPRITES));
}

void animateLava(void) {
  if ((_frame & 7) != 0) return;
  bool b = (_frame & 8) != 0;
  loadPat(T_LAVA, b ? TILE_LAVA1 : TILE_LAVA0);
  loadPat(T_LAVATOP, b ? TILE_LAVATOP1 : TILE_LAVATOP0);
  loadPat(T_FLAME, b ? TILE_FLAME1 : TILE_FLAME0);
}

void hideSprites(void) {
  vaddr(_vdpSpriteAttributeTableAddr);
  IO_VDPDATA = 0xD0;
}

// ---------------------------------------------------------------------------
// Lanes and players on screen
// ---------------------------------------------------------------------------
uint8_t laneRow(uint8_t p) { return _laneTop0 + p * LANE_H; }

// what a lane cell shows without a player on it
uint8_t laneBg(uint8_t col, uint8_t r) {
  if (col < LAVA_COLS) return r == 0 ? T_LAVATOP : T_LAVA;
  return r == LANE_H - 1 ? T_FLOOR : T_BLANK;
}

// move player p's 2x3 tiles to column col (0xFF = off the screen)
void placePlayer(uint8_t p, uint8_t col, bool direct) {
  Player *pl = &_pl[p];
  uint8_t row = laneRow(p);
  if (pl->col != 0xFF) {
    for (uint8_t r = 0; r < LANE_H; r++)
      for (uint8_t c = 0; c < 2; c++) {
        uint8_t id = laneBg(pl->col + c, r);
        if (direct) ntPut(pl->col + c, row + r, id);
        else _nt[(uint16_t)(row + r) * 32 + pl->col + c] = id;
      }
  }
  pl->col = col;
  if (col == 0xFF) return;
  for (uint8_t r = 0; r < LANE_H; r++)
    for (uint8_t c = 0; c < 2; c++) {
      uint8_t id = T_PLAYER + p * 6 + r * 2 + c;
      if (direct) ntPut(col + c, row + r, id);
      else _nt[(uint16_t)(row + r) * 32 + col + c] = id;
    }
}

uint8_t _pbuf[48];
uint8_t _cbuf[48];

// key: what the player looks like now
//   0 stand, 1-7 jump height, 17 duck, 18 hurt, 20-36 sinking into the lava, 40 gone
void drawPlayer(uint8_t p, uint8_t key, uint8_t color) {
  Player *pl = &_pl[p];
  const uint16_t *pic;
  uint8_t top = 7;
  uint8_t h = 16;
  bool lava = key >= 20;

  bool bot = pl->cpu;                      // CPU players are robots
  if (key == 17) { pic = bot ? PIC_RDUCK : PIC_DUCK; h = 8; top = 15; }
  else if (key == 18) pic = bot ? PIC_RHURT : PIC_HURT;
  else if (lava) { pic = bot ? PIC_RHURT : PIC_HURT; top = 7 + (key - 20); }
  else if (key == 0) pic = bot ? PIC_RSTAND : PIC_STAND;
  else { pic = bot ? PIC_RJUMP : PIC_JUMP; top = 7 - key; }

  for (uint8_t i = 0; i < 48; i++) _pbuf[i] = 0;
  for (uint8_t y = 0; y < h; y++) {
    uint8_t line = top + y;
    if (line >= 24) break;
    uint8_t o = (line >> 3) * 16 + (line & 7);
    uint16_t v = pic[y];
    _pbuf[o] = v >> 8;
    _pbuf[o + 8] = v & 0xFF;
  }
  if (!lava) { _pbuf[32 + 7] = 0xFF; _pbuf[32 + 15] = 0xFF; }   // the floor line under the feet

  bool newColor = color != pl->lastColor;
  if (newColor) {
    // in the lava: the lava red behind the player, below the surface (lane line 4)
    for (uint8_t i = 0; i < 48; i++)
      _cbuf[i] = (color << 4) | ((lava && (i >= 16 || (i & 7) >= 4)) ? VDP_MED_RED : 0);
    if (!lava) { _cbuf[32 + 7] = VDP_DARK_RED << 4; _cbuf[32 + 15] = VDP_DARK_RED << 4; }
    pl->lastColor = color;
  }
  uint8_t row = laneRow(p);
  for (uint8_t r = 0; r < LANE_H; r++) {
    uint8_t third = (row + r) >> 3;
    uint16_t base = (uint16_t)(T_PLAYER + p * 6 + r * 2) * 8 + third * 0x800;
    vaddr(_vdpPatternGeneratorTableAddr + base);
    vout(&_pbuf[r * 16], 16);
    if (newColor) {
      vaddr(_vdpColorTableAddr + base);
      vout(&_cbuf[r * 16], 16);
    }
  }
  pl->lastKey = key;
}

uint8_t playerLook(uint8_t p) {
  Player *pl = &_pl[p];
  if (pl->falling) {
    uint16_t d = (uint16_t)(_T16 - pl->fallStart) >> 4;
    return d > 16 ? 40 : 20 + (uint8_t)d;
  }
  if (pl->hurt) return 18;
  if (pl->act == ACT_JUMP) {
    uint16_t el = _T16 - pl->actStart;
    if (el >= JUMP_TICKS) return 1;
    return JUMP_H[el >> 2];
  }
  if (pl->act == ACT_DUCK) return 17;
  return 0;
}

uint8_t _gfxNext;
#define MAX_REDRAWS 3
void updatePlayerGfx(void) {
  uint8_t done = 0;
  for (uint8_t i = 0; i < _numPlayers && done < MAX_REDRAWS; i++) {
    uint8_t p = _gfxNext + i;
    if (p >= _numPlayers) p -= _numPlayers;
    Player *pl = &_pl[p];
    uint8_t key = playerLook(p);
    if (key == 40) {                       // sunk: take the tiles off the lane
      if (pl->col != 0xFF) {
        placePlayer(p, 0xFF, true);
        ntPut(SINK_COL, laneRow(p) + 1, T_LSKULL);
      }
      pl->lastKey = key;
      continue;
    }
    uint8_t color = pl->color;
    if (pl->hurt && (_frame & 4)) color = VDP_WHITE;
    if (pl->falling) color = VDP_BLACK;    // a silhouette sinking in the lava
    if (key != pl->lastKey || color != pl->lastColor) {
      drawPlayer(p, key, color);
      done++;
    }
  }
  if (++_gfxNext >= _numPlayers) _gfxNext = 0;
}

#define NAME_LEN 15
uint8_t _lanes;                  // lanes on the screen (a player who just joined has none yet)

// the player's name at the start of the lane, their wins below it (how far back they
// are shows on the lane)
void drawLabel(uint8_t p, bool direct) {
  Player *pl = &_pl[p];
  if (p >= _lanes) return;
  uint16_t o = (uint16_t)laneRow(p) * 32 + LABEL_COL;
  uint8_t i;
  for (i = 0; i < NAME_LEN; i++) _nt[o + i] = T_BLANK;
  for (i = 0; i < NAME_LEN && pl->name[i]; i++) _nt[o + i] = pl->name[i];
  if (direct) {
    vaddr(_vdpPatternNameTableAddr + o);
    vout(&_nt[o], NAME_LEN);
  }
}

// a name cut to 15 screen characters: upper case, anything the font lacks shown as ?
void cleanName(char *d, const char *s) {
  uint8_t i = 0;
  while (i < NAME_LEN && s[i]) {
    char ch = s[i];
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    else if (ch < 32 || ch > 95) ch = '?';
    d[i++] = ch;
  }
  d[i] = 0;
}

// a player's name: the one given, else P<n> / CPU<n>
void plName(uint8_t p, const char *s) {
  Player *pl = &_pl[p];
  if (s && s[0]) { cleanName(pl->name, s); return; }
  char *d = pl->name;
  if (pl->src == SRC_CPU || pl->cpu) { *d++ = 'C'; *d++ = 'P'; *d++ = 'U'; }
  else *d++ = 'P';
  *d++ = '0' + pl->num;
  *d = 0;
}

// "NAME" + text, centred on the message row
void nameMsg(uint8_t p, const char *t) {
  char m[33];
  uint8_t i = 0;
  const char *n = _pl[p].name;
  while (*n) m[i++] = *n++;
  while (*t && i < 32) m[i++] = *t++;
  m[i] = 0;
  clearRow(MSG_ROW, true);
  centerAt(MSG_ROW, m, true);
}

// above the lanes: each player's picture (a robot for a CPU) in their colour, with
// "WINS" and the count beside it
void drawWins(bool direct) {
  uint8_t pat[8];
  if (_laneTop0 < 6) return;               // (no room above the lanes)
  uint8_t row = _laneTop0 - 4;
  for (uint8_t p = 0; p < _numPlayers && p < _lanes; p++) {
    Player *pl = &_pl[p];
    const uint16_t *pic = pl->cpu ? PIC_RSTAND : PIC_STAND;
    uint8_t col = 1 + p * 8;
    for (uint8_t t = 0; t < 4; t++) {      // TL, TR, BL, BR
      for (uint8_t y = 0; y < 8; y++) {
        uint16_t v = pic[(t >> 1) * 8 + y];
        pat[y] = (t & 1) ? v & 0xFF : v >> 8;
      }
      loadTile(T_HEAD + p * 4 + t, pat, pl->color << 4);
    }
    uint16_t o = (uint16_t)row * 32 + col;
    _nt[o] = T_HEAD + p * 4;          _nt[o + 1] = T_HEAD + p * 4 + 1;
    _nt[o + 32] = T_HEAD + p * 4 + 2; _nt[o + 33] = T_HEAD + p * 4 + 3;
    _nt[o + 3] = 'W'; _nt[o + 4] = 'I'; _nt[o + 5] = 'N'; _nt[o + 6] = 'S';
    _nt[o + 32 + 3] = T_BLANK; _nt[o + 32 + 4] = '0' + pl->wins;
    _nt[o + 32 + 5] = T_BLANK; _nt[o + 32 + 6] = T_BLANK;
    if (direct) {
      vaddr(_vdpPatternNameTableAddr + o);
      vout(&_nt[o], 7);
      vaddr(_vdpPatternNameTableAddr + o + 32);
      vout(&_nt[o + 32], 7);
    }
  }
}

// a brick wall
void wallRow(uint8_t row) {
  for (uint8_t c = 0; c < 32; c++)
    _nt[(uint16_t)row * 32 + c] = T_BRICK;
}

void bigSkull(uint8_t col, uint8_t row) {
  uint16_t o = (uint16_t)row * 32 + col;
  _nt[o] = T_BIGSKULL;      _nt[o + 1] = T_BIGSKULL + 1;
  _nt[o + 32] = T_BIGSKULL + 2; _nt[o + 33] = T_BIGSKULL + 3;
}

// flames on row 20 over a lava sea on rows 21-23
void fireSea(void) {
  for (uint8_t c = 0; c < 32; c++) {
    _nt[20 * 32 + c] = T_FLAME;
    _nt[21 * 32 + c] = T_LAVATOP;
    _nt[22 * 32 + c] = T_LAVA;
    _nt[23 * 32 + c] = T_LAVA;
  }
}

void drawScene(void) {
  uint16_t i;
  for (i = 0; i < 768; i++) _nt[i] = T_BLANK;
  _laneTop0 = 3 + ((6 - _numPlayers) * LANE_H) / 2;   // centred in rows 3-20
  _lanes = _numPlayers;

  // lanes, with a brick wall above the first and below the last
  for (uint8_t p = 0; p < _numPlayers; p++)
    for (uint8_t r = 0; r < LANE_H; r++)
      for (uint8_t c = 0; c < 32; c++)
        _nt[(uint16_t)(laneRow(p) + r) * 32 + c] = laneBg(c, r);
  wallRow(_laneTop0 - 1);
  wallRow(laneRow(_numPlayers));
  for (i = 0; i < 32; i++) _nt[(uint16_t)(_laneTop0 - 2) * 32 + i] = T_FLAME;   // fire on the wall
  fireSea();
  for (uint8_t p = 0; p < _numPlayers; p++) {
    _pl[p].col = 0xFF;
    placePlayer(p, ALIVE(&_pl[p]) ? FRONT_COL - _pl[p].pos : FRONT_COL, false);
    drawLabel(p, false);
  }
  centerAt(0, "HOP OR DROP", false);
  drawWins(false);
  ntFlush();

  for (uint8_t p = 0; p < _numPlayers; p++) {
    _pl[p].lastKey = 0xFF;
    _pl[p].lastColor = 0xFF;
    drawPlayer(p, playerLook(p), _pl[p].color);
  }
}

// the call arrow at the right end of each lane
void drawCall(int8_t type) {
  uint8_t base = type < 0 ? T_BLANK : (type == HZ_LOW ? T_ARROWUP : T_ARROWDN);
  for (uint8_t p = 0; p < _numPlayers; p++) {
    uint8_t top = laneRow(p);
    for (uint8_t i = 0; i < 4; i++)
      ntPut(29 + (i & 1), top + (i >> 1), type < 0 ? T_BLANK : base + i);
  }
}

// ---------------------------------------------------------------------------
// Actions and judging
// ---------------------------------------------------------------------------
void netSendAct(uint8_t p);
void netSendHit(uint8_t p);

void startAct(Player *pl, uint8_t act) {
  pl->act = act;
  pl->actStart = _T16;
  pl->queued = ACT_NONE;
  if (_online && pl->src != SRC_NET) netSendAct(pl - _pl);
}

void request(uint8_t p, uint8_t act) {
  Player *pl = &_pl[p];
  if (!ALIVE(pl) || _gameState == GS_OVER) return;
  if (pl->act == ACT_NONE) {
    startAct(pl, act);
    return;
  }
  uint16_t dur = pl->act == ACT_JUMP ? JUMP_TICKS : DUCK_TICKS;
  uint16_t el = _T16 - pl->actStart;
  if (el + BUFFER_TICKS >= dur)
    pl->queued = act;
}

void updateActions(void) {
  for (uint8_t p = 0; p < _numPlayers; p++) {
    Player *pl = &_pl[p];
    if (pl->act != ACT_NONE) {
      uint16_t dur = pl->act == ACT_JUMP ? JUMP_TICKS : DUCK_TICKS;
      if ((uint16_t)(_T16 - pl->actStart) >= dur) {
        pl->act = ACT_NONE;
        if (pl->queued != ACT_NONE)
          startAct(pl, pl->queued);
      }
    }
    if (pl->hurt && (int16_t)(_T16 - pl->hurtUntil) >= 0)
      pl->hurt = false;
  }
}

bool isSafe(Player *pl, uint8_t type) {
  uint16_t el = _T16 - pl->actStart;
  if (type == HZ_LOW)
    return pl->act == ACT_JUMP && el >= JUMP_SAFE_FROM && el <= JUMP_SAFE_TO;
  return pl->act == ACT_DUCK && el >= DUCK_SAFE_FROM && el <= DUCK_SAFE_TO;
}

// a hit: one step back, the last one into the lava
void knockBack(uint8_t p) {
  Player *pl = &_pl[p];
  pl->pos++;
  sndHit();
  if (pl->pos >= START_STEPS) {
    _alive--;
    pl->survived = _playFrames - pl->joinFrame;
    pl->falling = true;
    pl->fallStart = _T16;
    pl->act = ACT_NONE;
    pl->queued = ACT_NONE;
    pl->hurt = false;
    pl->lastColor = 0xFF;
    placePlayer(p, SINK_COL, true);
  } else {
    pl->hurt = true;
    pl->hurtUntil = _T16 + 128;
    placePlayer(p, FRONT_COL - pl->pos, true);
  }
}

// ---------------------------------------------------------------------------
// Hazards
// ---------------------------------------------------------------------------
void spawnHazards(void) {
  while (_spawning && _nextHzBeat <= _beatNum + 4) {
    for (uint8_t i = 0; i < MAXHZ; i++) {
      if (!_hz[i].active) {
        _hz[i].active = true;
        _hz[i].hitBeat = _nextHzBeat;
        _hz[i].type = hzRnd() & 1;
        _hz[i].judged = 0;
        break;
      }
    }
    _hzCount++;
    if ((_hzCount % BPM_EVERY) == 0 && _bpm < MAX_BPM) {
      setBpm(_bpm + BPM_STEP);
    }
    // gap to the next one: 1 or 2 beats
    uint8_t r = hzRnd() & 15;
    _nextHzBeat += (r < 9) ? 1 : 2;
  }
}

void cpuPlan(Hazard *h);

void onBeat(void) {
  if (_gameState == GS_COUNTIN) {
    uint16_t b = _beatNum - _roundStartBeat;
    static const char *const CNT[4] = { "   3   ", "   2   ", "   1   ", "  GO!  " };
    if (b < 4) centerAt(MSG_ROW, CNT[b], true);
    if (b == 4) { clearRow(MSG_ROW, true); _gameState = GS_PLAY; }
  }
  spawnHazards();
  // call the hazard that hits on the next beat
  int8_t call = -1;
  for (uint8_t i = 0; i < MAXHZ; i++) {
    Hazard *h = &_hz[i];
    if (h->active && h->hitBeat == _beatNum + 1) {
      call = h->type;
      // (no call beep: the arrow is the call)
      cpuPlan(h);
    }
  }
  drawCall(call);
}

uint8_t _sat[4 * 32 + 1];

// The roller moves ROLL_PX px a beat through every lane at once. Its centre passes the
// front position (x FRONT_COL*8+8) on the hit beat; a player knocked back k steps stands 8k px
// further left and is reached STEP_TICKS(k) later.
void updateHazards(void) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAXHZ; i++) {
    Hazard *h = &_hz[i];
    if (!h->active) continue;
    int16_t e = (int16_t)(_T16 - (h->hitBeat << 8));
    if (e < -768) continue;
    for (uint8_t p = 0; p < _numPlayers; p++) {
      Player *pl = &_pl[p];
      uint8_t bit = 1 << p;
      if (!ALIVE(pl) || (h->judged & bit) || pl->src == SRC_NET || _gameState == GS_OVER) continue;
      if (e >= STEP_TICKS(pl->pos)) {
        h->judged |= bit;
        if (!isSafe(pl, h->type)) {
          knockBack(p);
          if (_online) netSendHit(p);
        }
      }
    }
    int16_t x = FRONT_COL * 8 - ROLL_X(e); // sprite left edge
    if (x < -16) { h->active = false; continue; }
    if (x > 255) continue;
    uint8_t color = h->type == HZ_LOW ? VDP_WHITE : VDP_GRAY;
    uint8_t xs = (uint8_t)x;
    if (x < 0) { xs = (uint8_t)(x + 32); color |= 0x80; }   // early clock
    uint8_t pat = (h->type == HZ_LOW ? 0 : 8) + (((_frame >> 2) & 1) << 2);
    uint8_t y = _laneTop0 * 8 + (h->type == HZ_LOW ? 9 : 2);
    for (uint8_t p = 0; p < _numPlayers && n < 4 * 32; p++, y += LANE_H * 8) {
      _sat[n++] = y - 1;
      _sat[n++] = xs;
      _sat[n++] = pat;
      _sat[n++] = color;
    }
  }
  _sat[n++] = 0xD0;
  // written after the judging above, which may update the name table
  vaddr(_vdpSpriteAttributeTableAddr);
  vout(_sat, n);
}

// ---------------------------------------------------------------------------
// CPU players
// ---------------------------------------------------------------------------
void cpuPlan(Hazard *h) {
  for (uint8_t p = 0; p < _numPlayers; p++) {
    Player *pl = &_pl[p];
    if (pl->src != SRC_CPU || !ALIVE(pl)) continue;
    uint16_t dur = h->type == HZ_LOW ? JUMP_TICKS : DUCK_TICKS;
    uint16_t cross = (h->hitBeat << 8) + STEP_TICKS(pl->pos);
    int16_t errT = (int16_t)pl->cpuErr * _ticksPerFrame;
    int16_t err = (int16_t)(rnd() % (uint16_t)(2 * errT + 1)) - errT;
    uint8_t act = h->type == HZ_LOW ? ACT_JUMP : ACT_DUCK;
    uint8_t r = rnd() & 31;
    if (r < 2) continue;                   // asleep: no press at all (1 in 16)
    if (r < 4) act = act == ACT_JUMP ? ACT_DUCK : ACT_JUMP;   // the wrong move (1 in 16)
    uint8_t k = pl->planAct[0] == ACT_NONE ? 0 : 1;
    pl->planAct[k] = act;
    pl->planTime[k] = cross - dur / 2 + err;
  }
}

void updateCpu(void) {
  for (uint8_t p = 0; p < _numPlayers; p++) {
    Player *pl = &_pl[p];
    if (pl->src != SRC_CPU) continue;
    for (uint8_t k = 0; k < 2; k++) {
      if (pl->planAct[k] != ACT_NONE && (int16_t)(_T16 - pl->planTime[k]) >= 0) {
        request(p, pl->planAct[k]);
        pl->planAct[k] = ACT_NONE;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
void readInput(void) {
  for (uint8_t s = 0; s < NUM_SRC; s++) {
    _srcJump[s] = 0;
    _srcDuck[s] = 0;
  }
  _escPressed = false;
  _anyKey = false;
  for (uint8_t j = 0; j < 4; j++) {
    uint8_t v = getJoyStatus(j);
    uint8_t nw = v & ~_lastJoy[j];
    if (nw & (Joy_Up | Joy_Button)) _srcJump[j] = 1;
    if (nw & Joy_Down) _srcDuck[j] = 1;
    if (nw) _anyKey = true;
    _lastJoy[j] = v;
  }
  while (isKeyPressed()) {
    uint8_t k = getChar();
    _anyKey = true;
    if (k == 0xE2) _srcJump[SRC_JOY1] = 1;          // the arrow keys work as joystick 1
    else if (k == 0xE3) _srcDuck[SRC_JOY1] = 1;
    else if (k == 0x1B) _escPressed = true;
    else if (k >= 0xE0) _anyKey = false;     // arrow releases etc.
  }
}

void humanInput(void) {
  for (uint8_t p = 0; p < _numPlayers; p++) {
    uint8_t s = _pl[p].src;
    if (s == SRC_ANY) {                    // online: any stick or keys
      for (s = 0; s < NUM_SRC; s++)
        if (_srcJump[s] || _srcDuck[s]) break;
      if (s == NUM_SRC) continue;
    }
    if (s >= NUM_SRC) continue;
    if (_srcJump[s]) request(p, ACT_JUMP);
    else if (_srcDuck[s]) request(p, ACT_DUCK);
  }
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
void tickClock(void) {
  uint16_t old = _beatFrac;
  _beatFrac += _fracInc;
  bool beat = _beatFrac < old;
  if (beat) _beatNum++;
  _T16 = (_beatNum << 8) | (_beatFrac >> 8);
  if (beat) onBeat();
}

bool netFrame(void);
bool netWaitStart(void);
void netSendEnd(void);
void localJoin(void);
void midJoinLocal(void);
void netCatchUp(void);

// the round is decided: offline and on the host, from who is still standing
// the last one standing wins the round (nobody when the last ones fall together); they
// play on for the leaderboard while they last
void decideWinner(void) {
  _winnerSet = true;
  _netWinner = 0xFF;
  for (uint8_t p = 0; p < _numPlayers; p++)
    if (ALIVE(&_pl[p])) _netWinner = p;
  if (_netWinner != 0xFF) nameMsg(_netWinner, " WINS THE ROUND!");
}

bool humanAlive(void) {
  for (uint8_t p = 0; p < _numPlayers; p++)
    if (!_pl[p].cpu && ALIVE(&_pl[p])) return true;
  return false;
}

// the round is over: everyone is in the lava, or only robots are left
void endRound(void) {
  if (!_winnerSet) decideWinner();
  _gameState = GS_OVER;
  _spawning = false;
  _overBeat = _beatNum;
  for (uint8_t p = 0; p < _numPlayers; p++)
    if (ALIVE(&_pl[p])) _pl[p].survived = _playFrames - _pl[p].joinFrame;
  _netMatchOver = _netWinner != 0xFF && _pl[_netWinner].wins + 1 >= WINS_NEEDED;
  if (_online) netSendEnd();
}

// mp_init for the leaderboard (online play has done it already)
bool mpReady(void) {
  if (_mpReady) return true;
  if (!_mpOk || _mpFailed) return false;
  if (mp_init(MP_GAME_ID) == MP_OK) _mpReady = true;
  else _mpFailed = true;
  return _mpReady;
}

// tenths of a second as "12.3"
char *fmtTime(uint16_t tenths, char *b) {
  char t[6];
  uint8_t i = 0, n = 0;
  uint16_t w = tenths / 10;
  do { t[n++] = '0' + w % 10; w /= 10; } while (w);
  while (n) b[i++] = t[--n];
  b[i++] = '.';
  b[i++] = '0' + tenths % 10;
  b[i] = 0;
  return b;
}

// after a round: each player's time under their name; the times of this NABU's named
// player go to the leaderboard
void roundTimes(void) {
  char b[12];
  uint8_t best = 0xFF;
  uint16_t rank = 0;
  for (uint8_t p = 0; p < _numPlayers && p < _lanes; p++) {
    Player *pl = &_pl[p];
    uint16_t o = (uint16_t)(laneRow(p) + 1) * 32 + LABEL_COL;
    fmtTime(pl->survived / 6, b);
    uint8_t i = 0;
    while (b[i]) { _nt[o + i] = b[i]; i++; }
    _nt[o + i] = 'S';
    vaddr(_vdpPatternNameTableAddr + o);
    vout(&_nt[o], i + 1);
  }
  if (_demo) return;
  for (uint8_t p = 0; p < _numPlayers; p++) {
    Player *pl = &_pl[p];
    bool mine = _online ? pl->src == SRC_ANY : (p == 0 && pl->src < NUM_SRC && _myName[0]);
    if (!mine || !_myName[0] || pl->survived < 60 || !mpReady()) continue;
    uint16_t r = 0;
    if (mp_scoreSubmit(LB_BOARD, pl->survived / 6, _myName, &r) == MP_OK && r) { best = p; rank = r; }
  }
  if (best != 0xFF) {
    char m[16] = " RANK ";
    u8str(rank > 255 ? 255 : rank, b);
    uint8_t i = 0;
    while (b[i]) { m[6 + i] = b[i]; i++; }
    m[6 + i] = 0;
    nameMsg(best, m);
  }
}

// one round; returns false when ESC was pressed (or the online session was lost)
bool playRound(void) {
  uint8_t p;

  for (p = 0; p < _numPlayers; p++) {
    _pl[p].pos = 0;
    _pl[p].act = ACT_NONE;
    _pl[p].queued = ACT_NONE;
    _pl[p].hurt = false;
    _pl[p].falling = false;
    _pl[p].planAct[0] = _pl[p].planAct[1] = ACT_NONE;
  }
  _alive = _numPlayers;
  for (uint8_t i = 0; i < MAXHZ; i++) _hz[i].active = false;

  setBpm(START_BPM);
  clockReset();
  _roundStartBeat = 1;
  _nextHzBeat = _roundStartBeat + 5;
  _hzCount = 0;
  _spawning = true;
  _gameState = GS_COUNTIN;
  _netEnd = false;
  _netWinner = 0xFF;
  _netMatchOver = false;
  _winnerSet = false;
  _playFrames = 0;
  for (p = 0; p < _numPlayers; p++) { _pl[p].survived = 0; _pl[p].joinFrame = 0; }
  // joining a round in progress (online): everyone's steps from the host's snapshot
  bool mid = _midJoin;
  _midJoin = false;
  if (mid && (int32_t)(_startTime - mp_time()) > 0) mid = false;   // (it hasn't started yet)
  if (mid)
    for (p = 0; p < _numPlayers; p++) {
      Player *pl = &_pl[p];
      pl->pos = _midPos[p] > START_STEPS ? START_STEPS : _midPos[p];
      if (!ALIVE(pl)) { _alive--; pl->falling = true; }
    }

  vdp_waitVDPReadyInt();
  hideSprites();
  drawScene();
  char b[24] = "ROUND ";
  char n[4];
  u8str(_round + 1, n);
  b[6] = n[0]; b[7] = n[1]; b[8] = 0;
  centerAt(MSG_ROW, b, true);

  // online: every NABU starts the beat clock at the same shared time (a mid-round joiner
  // runs the clock up to now instead)
  if (mid) netCatchUp();
  else if (_online && !netWaitStart()) { sndSilence(); hideSprites(); return false; }

  waitFrame();
  while (true) {
    uint8_t nf = waitFrame();
    _frame++;
    updateHazards();
    updatePlayerGfx();
    animateLava();
    for (uint8_t k = 0; k < nf; k++) {
      musicTick();
      sndFrame();
    }

    readInput();
    if (_escPressed || (_demo && _anyKey)) { sndSilence(); hideSprites(); return false; }
    if (_online && !netFrame()) { sndSilence(); hideSprites(); return false; }
    if (!_online && !_demo && _gameState != GS_OVER) midJoinLocal();

    for (uint8_t k = 0; k < nf; k++)
      tickClock();
    updateActions();
    if (_gameState == GS_PLAY) {
      _playFrames += nf;
      humanInput();
      updateCpu();
    }

    if (_gameState == GS_PLAY && _alive <= 1 && !_winnerSet)
      decideWinner();                      // (online the host's E has the final word)
    if (_gameState == GS_PLAY && (_alive == 0 || (_winnerSet && !humanAlive())) &&
        (!_online || _isHost))
      endRound();
    if (_online && _netEnd && _gameState != GS_OVER) {     // the host ended it
      _gameState = GS_OVER;
      _spawning = false;
      _overBeat = _beatNum;
      for (uint8_t q = 0; q < _numPlayers; q++)
        if (ALIVE(&_pl[q])) _pl[q].survived = _playFrames - _pl[q].joinFrame;
    }
    if (_gameState == GS_OVER && _beatNum >= _overBeat + 3)
      break;
  }

  // who won? (decided when the round ended; online the host's word)
  uint8_t winner = _netWinner;
  sndSilence();
  hideSprites();
  drawCall(-1);
  if (winner < _numPlayers) {
    _pl[winner].wins++;
    nameMsg(winner, " WINS THE ROUND!");
  } else {
    centerAt(MSG_ROW, "NOBODY SURVIVED!", true);
  }
  drawWins(true);
  roundTimes();
  // between rounds: new players can join (offline; online they join the session)
  bool canJoin = !_online && !_demo && !_netMatchOver;
  if (canJoin) centerAt(1, "PRESS HOP TO JOIN", true);
  for (uint16_t f = 0; f < 150; f++) {
    vdp_waitVDPReadyInt();
    _frame++;
    updatePlayerGfx();
    animateLava();
    readInput();
    if (_escPressed || (_demo && _anyKey)) return false;
    if (_online && !netFrame()) return false;
    if (canJoin) localJoin();
  }
  return true;
}

// a whole match; returns when someone has WINS_NEEDED or ESC
void playMatch(void) {
  for (uint8_t p = 0; p < _numPlayers; p++) _pl[p].wins = 0;
  _online = false;
  _hzRng = rnd() | 1;
  for (_round = 0; ; _round++) {
    if (!playRound()) return;
    for (uint8_t p = 0; p < _numPlayers; p++) {
      if (_pl[p].wins >= WINS_NEEDED) {
        nameMsg(p, " WINS THE MATCH!");
        for (uint16_t f = 0; f < 240; f++) {
          vdp_waitVDPReadyInt();
          _frame++;
          readInput();
          if (_escPressed) return;
        }
        return;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Lobby
// ---------------------------------------------------------------------------
const char *const SRC_NAME[NUM_SRC] = { "JOY 1/KEYS", "JOYSTICK 2", "JOYSTICK 3", "JOYSTICK 4" };
uint8_t _joined[NUM_SRC];        // order joined (0 = not)
uint8_t _numHumans;
uint8_t _numCpus;

void joinBeep(void) {
  tone(2, 76);
  ayWrite(10, 12);
  _volC = 12;
}

// a new player between rounds (offline): a free lane, else they take a CPU's place
void localJoin(void) {
  for (uint8_t s = 0; s < NUM_SRC; s++) {
    if (!_srcJump[s] || _joined[s]) continue;
    uint8_t p = 0xFF;
    for (uint8_t q = 0; q < _numPlayers; q++)
      if (_pl[q].src == SRC_CPU) p = q;    // the last CPU gives up its place
    bool took = p != 0xFF;
    if (took) _numCpus--;
    else {
      if (_numPlayers >= MAXP) return;
      p = _numPlayers;
      _numPlayers++;
      _pl[p].num = p + 1;
      _pl[p].color = PCOLOR[p];
    }
    _pl[p].src = s;
    _pl[p].cpu = false;
    _pl[p].wins = 0;
    _pl[p].lastKey = 0xFF;                 // redrawn as a human
    _joined[s] = ++_numHumans;
    plName(p, NULL);
    if (took) drawLabel(p, true);          // took a CPU's lane (a new lane shows next round)
    nameMsg(p, " JOINS NEXT ROUND");
    drawWins(true);
    joinBeep();
  }
}

// a robot still in the round whose lane a joining human can take: the one with the fewest
// steps back (never the round's winner); 0xFF = none
uint8_t freeRobot(void) {
  uint8_t best = 0xFF, bestPos = 0xFF;
  for (uint8_t p = 0; p < _numPlayers; p++) {
    Player *pl = &_pl[p];
    if (pl->src != SRC_CPU || !ALIVE(pl) || (_winnerSet && p == _netWinner)) continue;
    if (best == 0xFF || pl->pos < bestPos) { best = p; bestPos = pl->pos; }
  }
  return best;
}

// a human takes over robot p's lane during a round: the robot's steps stay, the human's
// time counts from now
void takeLane(uint8_t p, uint8_t src, uint8_t owner, const char *name) {
  Player *pl = &_pl[p];
  pl->src = src;
  pl->owner = owner;
  pl->cpu = false;
  pl->wins = 0;
  pl->joinFrame = _playFrames;
  pl->planAct[0] = pl->planAct[1] = ACT_NONE;
  pl->lastKey = 0xFF;                      // redrawn as a human
  plName(p, name);
  drawLabel(p, true);
  drawWins(true);
  nameMsg(p, " JOINS!");
  joinBeep();
}

// offline, during a round: HOP on a stick that isn't playing takes a robot's lane
void midJoinLocal(void) {
  for (uint8_t s = 0; s < 4; s++) {        // (the arrow keys are joystick 1)
    if (!_srcJump[s] || _joined[s]) continue;
    uint8_t p = freeRobot();
    if (p == 0xFF) return;                 // no robot left standing: next round
    _numCpus--;
    _joined[s] = ++_numHumans;
    takeLane(p, s, 0, NULL);
    _srcJump[s] = 0;                       // (the join press isn't a hop)
  }
}

// ---------------------------------------------------------------------------
// Menus: any stick or the arrow keys. UP/DOWN move the cursor, LEFT/RIGHT change a
// value, FIRE (or RIGHT on a command; RETURN and SPACE work too) chooses.
// ---------------------------------------------------------------------------
#define NAV_UP     1
#define NAV_DOWN   2
#define NAV_LEFT   4
#define NAV_RIGHT  8
#define NAV_FIRE   16
#define NAV_ESC    32
#define NAV_GO     (NAV_FIRE | NAV_RIGHT)
#define MENU_COL   10                // items (all menus start here); the cursor 2 to the left

uint8_t _navJoy[4];              // new presses per stick (FIRE on a free stick joins)

uint8_t navRead(void) {
  uint8_t r = 0;
  for (uint8_t j = 0; j < 4; j++) {
    uint8_t v = getJoyStatus(j);
    uint8_t nw = v & ~_lastJoy[j];
    _lastJoy[j] = v;
    _navJoy[j] = nw;
    if (nw & Joy_Up) r |= NAV_UP;
    if (nw & Joy_Down) r |= NAV_DOWN;
    if (nw & Joy_Left) r |= NAV_LEFT;
    if (nw & Joy_Right) r |= NAV_RIGHT;
    if (nw & Joy_Button) r |= NAV_FIRE;
  }
  while (isKeyPressed()) {
    uint8_t k = getChar();
    if (k == 0xE2) r |= NAV_UP;
    else if (k == 0xE3) r |= NAV_DOWN;
    else if (k == 0xE1) r |= NAV_LEFT;
    else if (k == 0xE0) r |= NAV_RIGHT;
    else if (k == ' ' || k == 13) { r |= NAV_FIRE; _navJoy[0] |= Joy_Button; }
    else if (k == 0x1B) r |= NAV_ESC;
  }
  return r;
}

// move the cursor over n items
uint8_t menuMove(uint8_t sel, uint8_t n, uint8_t nav) {
  uint8_t s = sel;
  if ((nav & NAV_UP) && s > 0) s--;
  if ((nav & NAV_DOWN) && s + 1 < n) s++;
  if (s != sel) joinBeep();
  return s;
}

// one menu line in _nt: the cursor, the text, and "< value >" when val is given
void menuLine(uint8_t row, bool on, const char *text, const char *val) {
  clearRow(row, false);
  if (on) _nt[(uint16_t)row * 32 + MENU_COL - 2] = '>';
  printAt(MENU_COL, row, text, false);
  if (val) {
    printAt(MENU_COL + 13, row, "<", false);
    printAt(MENU_COL + 15, row, val, false);
    printAt(MENU_COL + 19, row, ">", false);
  }
}

// the frame of a menu screen: title, music credit, the sea of fire
void menuFrame(void) {
  vdp_waitVDPReadyInt();
  hideSprites();
  for (uint16_t i = 0; i < 768; i++) _nt[i] = T_BLANK;
  centerAt(1, "H O P   O R   D R O P", false);
  bigSkull(1, 0);
  bigSkull(29, 0);
  centerAt(17, "MUSIC: EXIT THE PREMISES", false);
  centerAt(18, "KEVIN MACLEOD (INCOMPETECH.COM)", false);
  centerAt(19, "LICENSED UNDER CC BY 4.0", false);
  fireSea();
}

// a frame of a menu screen: the music and the fire
uint8_t menuTick(void) {
  uint8_t nf = waitFrame();
  _frame++;
  rnd();
  animateLava();
  for (uint8_t k = 0; k < nf; k++) { musicTick(); sndFrame(); }
  return navRead();
}

void flushRows(uint8_t from, uint8_t to) {
  vaddr(_vdpPatternNameTableAddr + from * 32);
  vout(&_nt[from * 32], (uint16_t)(to - from + 1) * 32);
}

// ---------------------------------------------------------------------------
// Offline lobby
// ---------------------------------------------------------------------------
#define LB_START   0
#define LB_BACK    1

void lobbyDraw(uint8_t sel) {
  char b[4];
  for (uint8_t r = 3; r < 16; r++) clearRow(r, false);
  printAt(4, 3, "PLAYERS", false);
  uint8_t row = 4;
  for (uint8_t k = 1; k <= _numHumans; k++) {
    for (uint8_t s = 0; s < NUM_SRC; s++) {
      if (_joined[s] == k) {
        if (k == 1 && _myLabel[0]) printAt(6, row, _myLabel, false);
        else {
          _nt[(uint16_t)row * 32 + 6] = 'P';
          _nt[(uint16_t)row * 32 + 7] = '0' + k;
        }
        printAt(22, row, SRC_NAME[s], false);
        row++;
      }
    }
  }
  for (uint8_t k = 0; k < _numCpus; k++) {
    printAt(6, row, "CPU", false);
    _nt[(uint16_t)row * 32 + 9] = '0' + _numHumans + k + 1;
    printAt(22, row, "ROBOT", false);
    row++;
  }
  bool freeStick = _numHumans < MAXP && !(_joined[0] && _joined[1] && _joined[2] && _joined[3]);
  if (freeStick) centerAt(9, "FIRE TO JOIN", false);
  (void)b;
  menuLine(11, sel == LB_START, "START", NULL);
  menuLine(12, sel == LB_BACK, "BACK", NULL);
  flushRows(3, 15);
}

// there are always MAXP players: robots in the places no human has taken
void lobbyJoin(uint8_t s) {
  _joined[s] = ++_numHumans;
  _numCpus = MAXP - _numHumans;
  joinBeep();
}

// returns 1 to start a match (players set up), 3 back to the menu, 0 for the demo
uint8_t lobby(void) {
  uint16_t idle = 0;
  uint8_t sel = LB_START;
  _numCpus = MAXP - _numHumans;
  menuFrame();
  ntFlush();
  lobbyDraw(sel);

  waitFrame();
  while (true) {
    uint8_t nav = menuTick();
    uint8_t old = sel;
    bool changed = false;
    bool any = nav != 0;
    // FIRE on a stick that hasn't joined: that stick joins
    for (uint8_t j = 0; j < 4; j++)
      if ((_navJoy[j] & Joy_Button) && !_joined[j] && _numHumans < MAXP) {
        lobbyJoin(j);
        nav &= ~NAV_FIRE;
        changed = true;
      }
    if (nav & NAV_ESC) return 3;
    sel = menuMove(sel, 2, nav);
    if (nav & NAV_GO) {
      if (sel == LB_BACK) return 3;
      break;
    }
    if (changed || sel != old) lobbyDraw(sel);
    if (any) idle = 0;
    else if (++idle > 60 * 20 && _numHumans == 0) return 0;
  }

  // players: humans in join order, then CPUs
  _numPlayers = 0;
  for (uint8_t k = 1; k <= _numHumans; k++)
    for (uint8_t s = 0; s < NUM_SRC; s++)
      if (_joined[s] == k) {
        _pl[_numPlayers].src = s;
        _numPlayers++;
      }
  for (uint8_t k = 0; k < _numCpus; k++) {
    _pl[_numPlayers].src = SRC_CPU;
    _numPlayers++;
  }
  for (uint8_t p = 0; p < _numPlayers; p++) {
    _pl[p].num = p + 1;
    _pl[p].color = PCOLOR[p];
    _pl[p].cpuErr = 3 + (rnd() % 5);
    _pl[p].cpu = _pl[p].src == SRC_CPU;
    plName(p, p == 0 ? _myName : NULL);   // the first player gets the adapter's name
  }
  return 1;
}

// ---------------------------------------------------------------------------
// Start menu: offline or online
// ---------------------------------------------------------------------------
uint8_t _menuSel;                // 0 online, 1 offline, 2 leaderboard

void menuDraw(void) {
  menuLine(7, _menuSel == 0, "ONLINE", NULL);
  menuLine(9, _menuSel == 1, "OFFLINE", NULL);
  menuLine(11, _menuSel == 2, "LEADERBOARD", NULL);
  flushRows(7, 11);
}

// returns 1 offline, 2 online, 3 leaderboard, 0 for the demo (20 s idle)
uint8_t menu(void) {
  uint16_t idle = 0;
  menuFrame();
  if (_myLabel[0]) {
    char h[24] = "HELLO ";
    uint8_t i = 0;
    while (_myLabel[i]) { h[6 + i] = _myLabel[i]; i++; }
    h[6 + i] = 0;
    centerAt(5, h, false);
  }
  // credits: the jobs from column 2, the names 2 after the longest one's colon
  for (uint8_t r = 13; r < 20; r++) clearRow(r, false);
  printAt(2, 13, "DIRECTOR:", false);  printAt(13, 13, "GTAMP", false);
  printAt(2, 14, "CODE:", false);      printAt(13, 14, "CLAUDE", false);
  printAt(2, 15, "SERVER:", false);    printAt(13, 15, "NABU.CA", false);
  printAt(2, 16, "MUSIC:", false);     printAt(13, 16, "EXIT THE PREMISES", false);
  printAt(13, 17, "KEVIN MACLEOD", false);
  printAt(13, 18, "(INCOMPETECH.COM)", false);
  printAt(13, 19, "CC BY 4.0", false);
  ntFlush();
  menuDraw();

  waitFrame();
  while (true) {
    uint8_t nav = menuTick();
    uint8_t s = menuMove(_menuSel, 3, nav);
    if (s != _menuSel) { _menuSel = s; menuDraw(); }
    if (nav & NAV_GO) return _menuSel == 0 ? 2 : _menuSel == 1 ? 1 : 3;
    if (nav) idle = 0;
    else if (++idle > 60 * 20) return 0;
  }
}

// demo: 4 CPUs, any key returns
void demo(void) {
  _numPlayers = 4;
  for (uint8_t p = 0; p < 4; p++) {
    _pl[p].src = SRC_CPU;
    _pl[p].num = p + 1;
    _pl[p].color = PCOLOR[p];
    _pl[p].cpuErr = 3 + (rnd() % 5);
    _pl[p].wins = 0;
    _pl[p].cpu = true;
    plName(p, NULL);
  }
  _hzRng = rnd() | 1;
  _round = 0;
  _demo = true;
  while (playRound())
    _round++;
  _demo = false;
}

// ---------------------------------------------------------------------------
// Online (RetroNET-Multiplayer.h)
//
// One player per NABU; CPU robots fill the other places, run by the host's NABU. Packets,
// all on the reliable channel to everyone else:
//   'R' round, seed lo, seed hi, start time (4, mp_time ms), n, n x (owner, cpu, wins)
//       the host, before each round. The player list is made from the session at that
//       moment, so anyone who joined during the last round plays in the next one.
//   'A' round, slot, act, tick lo, tick hi    a player started a jump / duck
//   'H' round, slot, pos                      a player was hit (pos = steps back)
//   'E' round, winner, match over             the host ended the round
//   'J'                                       to the host: a waiting player asks to join
//                                             the round in progress
//   'S' round, seed lo, seed hi, start time (4), n, n x (owner, cpu, wins, pos), slot
//       the host: that player has taken robot slot's lane; the joiner runs the round's
//       clock and hazards from the start time to now and plays from there
// Each NABU judges only its own players; everyone sees the same hazards because they
// come from the round's seed and the beat clock, started at the same shared time.
// ---------------------------------------------------------------------------
void netSend(uint8_t len) {
  mp_send(MP_TARGET_OTHERS, MP_CHANNEL_RELIABLE, len, _tx);
}

void netSendAct(uint8_t p) {
  _tx[0] = 'A'; _tx[1] = _round; _tx[2] = p; _tx[3] = _pl[p].act;
  _tx[4] = _pl[p].actStart & 0xFF; _tx[5] = _pl[p].actStart >> 8;
  netSend(6);
}

void netSendHit(uint8_t p) {
  _tx[0] = 'H'; _tx[1] = _round; _tx[2] = p; _tx[3] = _pl[p].pos;
  netSend(4);
}

void netSendEnd(void) {
  _tx[0] = 'E'; _tx[1] = _round; _tx[2] = _netWinner; _tx[3] = _netMatchOver;
  netSend(4);
}

void netJoinReq(uint8_t id);
void netGotS(uint8_t len);

void netHandle(uint8_t len) {
  uint8_t *b = _rx;
  if (b[0] == 'J') {
    if (_isHost) netJoinReq(_rxFrom);
    return;
  }
  if (b[0] == 'S') {
    netGotS(len);
    return;
  }
  if (b[0] == 'R') {
    if (len >= 9 && len <= sizeof(_rbuf)) {
      memcpy(_rbuf, b, len);
      _haveR = true;
    }
    return;
  }
  if (!_inRound || len < 4 || b[1] != _round) return;     // an old round's packet
  if (b[0] == 'E') {
    _netEnd = true;
    _netWinner = b[2];
    _netMatchOver = b[3] != 0;
    return;
  }
  uint8_t slot = b[2];
  if (slot >= _numPlayers) return;
  Player *pl = &_pl[slot];
  if (pl->src != SRC_NET || !ALIVE(pl)) return;
  if (b[0] == 'A' && len >= 6) {
    pl->act = b[3];
    pl->actStart = b[4] | ((uint16_t)b[5] << 8);
    pl->queued = ACT_NONE;
  } else if (b[0] == 'H') {
    while (ALIVE(pl) && pl->pos < b[3]) knockBack(slot);
  }
}

// once a frame online: take waiting packets, now and then the session's status.
// false = the session is gone.
bool netFrame(void) {
  for (uint8_t k = 0; k < 4; k++) {
    uint16_t n = mp_recv(_rx, sizeof(_rx), &_rxFrom, NULL);
    if (!n) break;
    netHandle((uint8_t)n);
  }
  if ((_frame & 31) == 0) {
    mp_status(&_mst);
    if (_mst.state != MP_STATE_IN_GAME && _mst.state != MP_STATE_LOBBY) return false;
    _isHost = (_mst.flags & MP_STATUS_IS_HOST) != 0;   // (the host may have left: migration)
    _myId = _mst.myId;
    // a player whose NABU left the session (or the old host's CPUs) carries on as a
    // CPU run by the host
    if (_inRound)
      for (uint8_t p = 0; p < _numPlayers; p++) {
        Player *pl = &_pl[p];
        if (pl->src == SRC_NET && !(_mst.playerMask & (1u << pl->owner))) {
          pl->owner = _mst.hostId;
          if (!pl->cpu) {
            pl->cpu = true;
            plName(p, NULL);
            drawLabel(p, true);
            drawWins(true);
            pl->lastKey = 0xFF;            // redrawn as a robot
          }
          if (pl->owner == _myId) {
            pl->src = SRC_CPU;
            pl->planAct[0] = pl->planAct[1] = ACT_NONE;
          }
        }
      }
  }
  return true;
}

// a full-screen message (with the fire below)
void netScreen(const char *a, const char *b) {
  sndSilence();                            // (no note held through a wait on the network)
  hideSprites();
  for (uint16_t i = 0; i < 768; i++) _nt[i] = T_BLANK;
  centerAt(1, "H O P   O R   D R O P", false);
  bigSkull(1, 0);
  bigSkull(29, 0);
  centerAt(9, a, false);
  centerAt(11, b, false);
  fireSea();
  ntFlush();
}

void waitKeyOrTime(uint16_t frames) {
  for (uint16_t f = 0; f < frames; f++) {
    uint8_t nf = waitFrame();
    _frame++;
    animateLava();
    for (uint8_t k = 0; k < nf; k++) { musicTick(); sndFrame(); }
    readInput();
    if (_anyKey && f > 30) return;
  }
}

// an error from the adapter: its short name and its own explanation
void netError(uint8_t r) {
  char t[64];
  char a[32] = "ONLINE: ";
  const char *n = mp_resultName(r);
  uint8_t i = 8;
  while (*n && i < 31) a[i++] = *n++;
  a[i] = 0;
  mp_getLastError((uint8_t *)t);
  t[63] = 0;
  for (i = 0; t[i]; i++) if (t[i] < 32 || t[i] > 126) t[i] = ' ';
  netScreen(a, "");
  // the explanation over two rows
  if (i > 30) { char c = t[30]; t[30] = 0; centerAt(13, t, true); t[30] = c; centerAt(14, t + 30, true); }
  else centerAt(13, t, true);
  waitKeyOrTime(60 * 6);
}

// wait for the round's start time: one mp_time() call, then counting frames here (calls
// mask the keyboard interrupt, so they are kept few); the clock starts as far in as the
// wait overshot
bool netWaitStart(void) {
  int32_t wait = (int32_t)(_startTime - mp_time());
  if (wait > 6000) wait = 6000;            // (a round packet from far in the future: don't hang)
  int16_t frames = wait > 0 ? (int16_t)((wait * 60) / 1000) : 0;
  int16_t late = wait < 0 ? (int16_t)(wait < -500 ? 500 : -wait) : 0;
  while (frames > 0) {
    frames -= waitFrame();
    _frame++;
    animateLava();
    readInput();
    if (_escPressed || !netFrame()) return false;
  }
  if (frames < 0) late += (int16_t)(-frames * 1000 / 60);
  _beatFrac = (uint16_t)(((uint32_t)late * START_BPM * 65536) / 60000);
  _T16 = _beatFrac >> 8;
  return true;
}

// a mid-round joiner: run the round's beat clock and hazards from its start time to now
// (nothing judged, nothing drawn but the calls), then play from there. The hazards come
// from the seed and the beat clock alone, so they match everyone else's.
void netCatchUp(void) {
  uint32_t done = 0;
  for (uint8_t pass = 0; pass < 4; pass++) {      // (catching up takes time too)
    int32_t ms = (int32_t)(mp_time() - _startTime);
    uint32_t target = ms > 0 ? (uint32_t)ms * 60 / 1000 : 0;
    if (target <= done) break;
    while (done < target) {
      for (uint8_t i = 0; i < MAXHZ; i++) {        // gone off the left, as updateHazards
        Hazard *h = &_hz[i];
        if (!h->active) continue;
        int16_t e = (int16_t)(_T16 - (h->hitBeat << 8));
        if (e >= -768 && FRONT_COL * 8 - ROLL_X(e) < -16) h->active = false;
      }
      tickClock();
      if (_gameState == GS_PLAY) _playFrames++;
      done++;
    }
  }
  // the hazards already close aren't judged for anyone (the joiner hasn't seen them)
  for (uint8_t i = 0; i < MAXHZ; i++)
    if (_hz[i].active && _hz[i].hitBeat <= _beatNum + 2) _hz[i].judged = 0xFF;
  for (uint8_t p = 0; p < _numPlayers; p++) {
    Player *pl = &_pl[p];
    if (pl->src == SRC_ANY) {
      pl->joinFrame = _playFrames;
      nameMsg(p, " JOINS!");
    }
    if (pl->falling) pl->fallStart = _T16 - 0x1000;   // long gone into the lava
  }
}

// host: player id wants to join the round in progress. They take a robot's lane (if
// one is still standing) and everyone gets the snapshot.
void netJoinReq(uint8_t id) {
  uint8_t b[32];
  if (!_inRound || _gameState == GS_OVER || id >= 16 || id == _myId) return;
  uint8_t p;
  for (p = 0; p < _numPlayers; p++)
    if (!_pl[p].cpu && _pl[p].owner == id) break;     // asked again: send it again
  if (p == _numPlayers) {
    p = freeRobot();
    if (p == 0xFF) return;                 // no robot standing: they wait for the next round
    MpPlayerInfo pi;
    mp_getPlayer(id, &pi);
    takeLane(p, SRC_NET, id, (const char *)pi.name);
  }
  uint8_t n = _numPlayers, o = 9;
  b[0] = 'S'; b[1] = _round;
  b[2] = _roundSeed & 0xFF; b[3] = _roundSeed >> 8;
  b[4] = _startTime & 0xFF; b[5] = (_startTime >> 8) & 0xFF;
  b[6] = (_startTime >> 16) & 0xFF; b[7] = _startTime >> 24;
  b[8] = n;
  for (uint8_t q = 0; q < n; q++) {
    Player *pl = &_pl[q];
    b[o] = pl->owner; b[o + 1] = pl->cpu; b[o + 2] = pl->wins; b[o + 3] = pl->pos;
    o += 4;
  }
  b[o++] = p;
  mp_send(MP_TARGET_OTHERS, MP_CHANNEL_RELIABLE, o, b);
}

// a snapshot: ours to play (while waiting), else someone took a robot's lane
void netGotS(uint8_t len) {
  uint8_t *b = _rx;
  uint8_t n = b[8];
  if (n > MAXP || n == 0 || len < 10 + n * 4) return;
  uint8_t slot = b[9 + n * 4];
  if (slot >= n) return;
  uint8_t owner = b[9 + slot * 4];
  if (owner == _myId) {
    if (!_inRound) { memcpy(_sbuf, b, len); _haveS = true; }
    return;
  }
  if (!_inRound || b[1] != _round || slot >= _numPlayers) return;
  Player *pl = &_pl[slot];
  if (!pl->cpu && pl->owner == owner) return;          // (seen it)
  MpPlayerInfo pi;
  mp_getPlayer(owner, &pi);
  takeLane(slot, SRC_NET, owner, (const char *)pi.name);
}

// the player's wins in this match so far (0 for someone new)
uint8_t oldWins(uint8_t id, bool cpu, uint8_t k) {
  for (uint8_t p = 0; p < _numPlayers; p++) {
    Player *pl = &_pl[p];
    if (pl->cpu != cpu) continue;
    if (cpu ? k-- == 0 : pl->owner == id) return pl->wins;
  }
  return 0;
}

// host: the next round from who is in the session now
void netHostRound(void) {
  uint8_t *b = _rbuf;
  uint8_t n = 0, o = 9;
  mp_status(&_mst);
  for (uint8_t id = 0; id < 16 && n < MAXP; id++) {
    if (!(_mst.playerMask & (1u << id))) continue;
    b[o] = id; b[o + 1] = 0; b[o + 2] = oldWins(id, false, 0);
    o += 3; n++;
  }
  // the same number of players as before: CPUs fill the places of humans who left,
  // and give theirs up to humans who joined
  for (uint8_t k = 0; n < MAXP; k++) {     // always 4: robots in the empty places
    b[o] = _mst.myId; b[o + 1] = 1; b[o + 2] = oldWins(0, true, k);
    o += 3; n++;
  }
  uint16_t seed = rnd() | 1;
  uint32_t t = mp_time() + 3000;           // time for the packet to reach everyone
  b[0] = 'R'; b[1] = _netRound++;
  b[2] = seed & 0xFF; b[3] = seed >> 8;
  b[4] = t & 0xFF; b[5] = (t >> 8) & 0xFF; b[6] = (t >> 16) & 0xFF; b[7] = t >> 24;
  b[8] = n;
  mp_send(MP_TARGET_OTHERS, MP_CHANNEL_RELIABLE, o, b);
  _haveR = true;
  char st[12] = "ROUND ";
  char nb[4];
  u8str(b[1] + 1, nb);
  st[6] = nb[0]; st[7] = nb[1]; st[8] = 0;
  mp_setStatusText(st);
}

// everyone: set the players up from the round packet
void netApplyRound(uint8_t *b, uint8_t stride) {
  MpPlayerInfo pi;
  _haveR = false;
  _haveS = false;
  _round = b[1];
  _roundSeed = b[2] | ((uint16_t)b[3] << 8);
  _hzRng = _roundSeed | 1;
  _startTime = b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
  uint8_t n = b[8] > MAXP ? MAXP : b[8];
  _numPlayers = n;
  for (uint8_t p = 0; p < n; p++) {
    Player *pl = &_pl[p];
    uint8_t *e = &b[9 + p * stride];
    _midPos[p] = stride > 3 ? e[3] : 0;
    pl->owner = e[0];
    pl->cpu = e[1] != 0;
    pl->wins = e[2];
    pl->num = p + 1;
    pl->color = PCOLOR[p];
    pl->cpuErr = 3 + (rnd() % 5);
    if (pl->owner == _myId) pl->src = pl->cpu ? SRC_CPU : SRC_ANY;
    else pl->src = SRC_NET;
    if (pl->cpu) plName(p, NULL);
    else {
      mp_getPlayer(pl->owner, &pi);
      plName(p, (const char *)pi.name);
    }
  }
  _inRound = true;
}

// a client between rounds: 0 = left, 1 = a round came, 2 = the match is over (lobby),
// 3 = we became the host
uint8_t netWaitRound(void) {
  netScreen("WAITING FOR THE NEXT ROUND", "");
  menuLine(13, true, "LEAVE", NULL);
  flushRows(13, 13);
  for (uint16_t f = 0; ; f++) {
    uint8_t nav = menuTick();
    if ((nav & NAV_ESC) || ((nav & NAV_GO) && f > 60) || !netFrame()) return 0;
    if (_haveR || _haveS) return 1;
    if ((f & 63) == 20) {                  // ask the host for a robot's lane, every ~1 s
      _tx[0] = 'J';
      mp_send(MP_TARGET_HOST, MP_CHANNEL_RELIABLE, 1, _tx);
    }
    if ((_frame & 31) == 0) {
      if (_mst.state == MP_STATE_LOBBY) return 2;
      if (_isHost) return 3;
    }
  }
}

// rounds until someone has WINS_NEEDED; false = left the session
bool netMatch(void) {
  _numPlayers = 0;                         // (no wins carried in from an old match)
  _netRound = 0;
  _inRound = false;
  _haveS = false;
  while (true) {
    if (!_haveR && !_haveS) {
      if (_isHost) {
        netHostRound();
      } else {
        uint8_t r = netWaitRound();
        if (r == 0) return false;
        if (r == 2) return true;
        continue;
      }
    }
    if (_haveR) netApplyRound(_rbuf, 3);
    else { netApplyRound(_sbuf, 4); _midJoin = true; }   // into the round in progress
    bool ok = playRound();
    _inRound = false;
    if (!ok) return false;
    if (_netMatchOver && _netWinner < _numPlayers) {
      nameMsg(_netWinner, " WINS THE MATCH!");
      for (uint16_t f = 0; f < 240; f++) {
        vdp_waitVDPReadyInt();
        _frame++;
        readInput();
        if (_escPressed || !netFrame()) return false;
      }
      if (_isHost) {
        Player *w = &_pl[_netWinner];
        mp_end(w->cpu ? 0 : (1u << w->owner), MP_END_FLAG_TO_LOBBY);
        mp_setStatusText("LOBBY");
      }
      return true;
    }
  }
}

// the session's lobby menu: the host has START, LEAVE; the others LEAVE
uint8_t netLobbyItems(void) { return _isHost ? 2 : 1; }

void netLobbyDraw(uint8_t sel) {
  MpPlayerInfo pi;
  char b[4];
  uint8_t r, humans = 0;
  for (r = 3; r < 17; r++) clearRow(r, false);
  centerAt(3, "ONLINE", false);
  printAt(4, 5, "PLAYERS", false);
  r = 6;
  for (uint8_t id = 0; id < 16; id++) {
    if (!(_mst.playerMask & (1u << id))) continue;
    mp_getPlayer(id, &pi);
    char nm[16];
    cleanName(nm, (const char *)pi.name);
    if (r < 10) {
      printAt(4, r, nm[0] ? nm : "?", false);
      if (pi.flags & MP_PLAYER_HOST) printAt(20, r, "HOST", false);
      if (id == _mst.myId) printAt(25, r, "(YOU)", false);
      r++;
    }
    humans++;
  }
  (void)b;
  for (; r < 6 + MAXP; r++) printAt(4, r, "CPU ROBOT", false);   // robots fill the 4 places
  if (_isHost) {
    menuLine(11, sel == 0, "START", NULL);
    menuLine(12, sel == 1, "LEAVE", NULL);
  } else {
    centerAt(11, "WAITING FOR THE HOST", false);
    menuLine(13, true, "LEAVE", NULL);
  }
  flushRows(3, 16);
}

// the session's lobby, until we leave it
void netLobby(void) {
  uint8_t rosterRev = 0, dataRev = 0, sel = 0;
  bool redraw = true, wasHost = false;
  uint8_t sd[MP_SESSION_DATA_SIZE];
  menuFrame();
  ntFlush();
  mp_status(&_mst);
  rosterRev = _mst.rosterRev + 1;          // (draw the first time)
  dataRev = _mst.dataRev + 1;
  while (true) {
    uint8_t nav = menuTick();
    if ((_frame & 7) == 0) {
      mp_status(&_mst);
      if (_mst.state != MP_STATE_LOBBY && _mst.state != MP_STATE_IN_GAME) {
        netScreen("THE SESSION HAS CLOSED", "");
        waitKeyOrTime(60 * 3);
        return;
      }
      _isHost = (_mst.flags & MP_STATUS_IS_HOST) != 0;
      _myId = _mst.myId;
      if (_mst.state == MP_STATE_IN_GAME) {   // started (or we joined a game in progress)
        if (!netMatch()) return;
        mp_flush();                        // (nothing of that match is wanted now)
        menuFrame();
        ntFlush();
        redraw = true;
        continue;
      }
      if (_mst.dataRev != dataRev) {
        dataRev = _mst.dataRev;
        mp_getSessionData(sd);
        redraw = true;
      }
      if (_mst.rosterRev != rosterRev || _isHost != wasHost) {
        rosterRev = _mst.rosterRev;
        wasHost = _isHost;
        if (sel >= netLobbyItems()) sel = 0;
        redraw = true;
      }
    }
    if (nav & NAV_ESC) return;
    uint8_t old = sel;
    sel = menuMove(sel, netLobbyItems(), nav);
    if (sel != old) redraw = true;
    bool leave = _isHost ? sel == 1 : true;
    if (leave) {
      if (nav & NAV_GO) return;
    } else if (nav & NAV_GO) {             // host: START
      sndSilence();
      uint8_t r = mp_start();
      if (r != MP_OK) { netError(r); menuFrame(); ntFlush(); }
      redraw = true;                       // (the next status check sees IN_GAME)
    }
    if (redraw) {
      netLobbyDraw(sel);
      redraw = false;
    }
  }
}

// ONLINE PLAY in the start menu: connect, quick join (or make) a session, play, leave
void netPlay(void) {
  _isHost = false;
  _haveR = false;
  _inRound = false;
  if (!_mpOk) {
    netScreen("ONLINE PLAY NEEDS A NEWER", "INTERNET ADAPTER");
    waitKeyOrTime(60 * 4);
    return;
  }
  netScreen("CONNECTING...", "");
  uint8_t r = mp_init(MP_GAME_ID);
  if (r != MP_OK) { netError(r); return; }
  _mpReady = true;
  uint16_t sid = 0;
  bool created = false;
  const char *nm = _myName[0] ? _myName : "PLAYER";
  uint8_t ver = GAME_VERSION;
  // an open session of this version (session data byte 0 = its GAME_VERSION) to join
  uint8_t cnt = 0;
  bool newer = false;
  r = mp_listSessions(MP_LIST_ALL, &cnt);
  if (r != MP_OK) {
    netError(r);
    mp_shutdown();
    return;
  }
  for (uint8_t i = 0; i < cnt && !sid; i++) {
    mp_getSessionInfo(i, &_si);
    if (!_si.sessionId || (_si.flags & (MP_INFO_NEEDS_CODE | MP_INFO_LOCKED)) ||
        _si.playerCount >= _si.maxPlayers) continue;
    if (_si.state != MP_STATE_LOBBY &&
        !(_si.state == MP_STATE_IN_GAME && (_si.flags & MP_INFO_LATE_JOIN))) continue;
    if (_si.sessionData[0] == GAME_VERSION) sid = _si.sessionId;
    else if (_si.sessionData[0] > GAME_VERSION) newer = true;
  }
  if (sid) r = mp_join(sid, NULL, nm, &_myId);
  if (!sid || r != MP_OK) {
    if (newer) {                           // the others have a newer HOP OR DROP
      mp_shutdown();
      netScreen("PLEASE UPDATE HOP OR DROP", "THE OTHERS HAVE A NEWER ONE");
      waitKeyOrTime(60 * 6);
      return;
    }
    // none: host a new one, announced on nabu.ca and Discord as "NAME's inferno"
    char title[24];
    uint8_t i = 0;
    while (nm[i] && i < 13) { title[i] = nm[i]; i++; }
    const char *t = "'s inferno";
    while (*t) title[i++] = *t++;
    title[i] = 0;
    r = mp_create(MAXP, MP_FLAG_LATE_JOIN | MP_FLAG_HOST_MIGRATION, NULL, nm, title,
                  "Hop or drop. Join any time!", &sid, &_myId);
    created = true;
  }
  if (r != MP_OK) {
    netError(r);
    mp_shutdown();
    return;
  }
  if (created) mp_setSessionData(0, 1, &ver);
  _online = true;
  netLobby();
  _online = false;
  mp_leave();
  mp_shutdown();
  _mpReady = false;
}

// the leaderboard: the top 10 times
void showLeaderboard(void) {
  MpScoreEntry e;
  char b[12];
  uint8_t count = 0, r;
  if (!_mpOk) {
    netScreen("THE LEADERBOARD NEEDS A", "NEWER INTERNET ADAPTER");
    waitKeyOrTime(60 * 4);
    return;
  }
  netScreen("LOADING THE LEADERBOARD...", "");
  _mpFailed = false;
  if (!mpReady()) {
    netScreen("THE LEADERBOARD IS NOT", "AVAILABLE");
    waitKeyOrTime(60 * 4);
    return;
  }
  r = mp_scoreCount(LB_BOARD, &count);
  if (r != MP_OK) { netError(r); return; }
  netScreen("", "");
  centerAt(3, "LONGEST SURVIVORS", true);
  if (!count) centerAt(9, "NO TIMES YET", true);
  for (uint8_t i = 0; i < count && i < 10; i++) {
    mp_scoreGet(LB_BOARD, i, &e);
    if (!e.rank) break;
    char nm[16];
    cleanName(nm, (const char *)e.name);
    printAt(3, 5 + i, u8str(e.rank > 255 ? 255 : e.rank, b), true);
    printAt(7, 5 + i, nm, true);
    printAt(23, 5 + i, fmtTime(e.score > 65535 ? 65535 : (uint16_t)e.score, b), true);
  }
  menuLine(16, true, "BACK", NULL);
  flushRows(16, 16);
  waitFrame();
  while (true) {
    uint8_t nav = menuTick();
    if (nav & (NAV_GO | NAV_ESC)) return;
  }
}


// an HCCA byte, or -1 after `frames` vsyncs without one (interrupts stay on, so the
// keyboard and the vsync counter keep working)
int16_t hccaReadTimeout(uint8_t frames) {
  uint8_t start = vsyncCount;
  while (!hcca_isRxBufferAvailable())
    if ((uint8_t)(vsyncCount - start) >= frames) return -1;
  return hcca_readByte();
}

// mp_isSupported() without the hang: asks the adapter's version (IA Control BA 16) and
// gives up after a second. True when it is at least MP_MIN_IA_VERSION.
bool adapterCheck(void) {
  char v[14];
  const char *minV = MP_MIN_IA_VERSION;
  uint8_t i;
  memset(v, 0, sizeof(v));
  while (hcca_isRxBufferAvailable()) hcca_readByte();     // (nothing stale)
  hcca_writeByte(0xBA);
  hcca_writeByte(0x16);
  int16_t len = hccaReadTimeout(60);
  if (len < 0) return false;
  for (i = 0; i < (uint8_t)len; i++) {
    int16_t c = hccaReadTimeout(30);
    if (c < 0) return false;
    if (i < 13) v[i] = (char)c;
  }
  for (i = 0; i < 13 && minV[i]; i++) {
    if ((uint8_t)v[i] > (uint8_t)minV[i]) return true;
    if ((uint8_t)v[i] < (uint8_t)minV[i]) return false;
  }
  return true;
}

void main(void) {
  initNABULib();
  vdp_initG2Mode(VDP_BLACK, true, false, false, true);
  vdp_addISR(frameISR);
  loadGraphics();
  sndInit();
  musicReset();                  // the song plays on through the lobby and the rounds
  _numCpus = 3;
  // the player name from the adapter, if it is new enough for multiplayer (an old one
  // may not answer the version question at all: then the game just stays offline)
  _mpOk = adapterCheck();
  if (_mpOk) {
    mp_getPlayerName((uint8_t *)_myName);
    cleanName(_myLabel, _myName);
  }

  while (true) {
    uint8_t m = menu();
    if (m == 2) netPlay();
    else if (m == 3) showLeaderboard();
    else if (m == 0) demo();
    else
      while (true) {             // offline: the lobby until ESC
        uint8_t r = lobby();
        if (r == 1) playMatch();
        else if (r == 0) demo();
        else break;
        sndSilence();
      }
    sndSilence();
  }
}
