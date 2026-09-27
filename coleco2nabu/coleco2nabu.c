#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <stdint.h>
#include <ctype.h>

#define MAX_ROM_SIZE 0x8000 
#define STUB_SIZE 15

typedef struct {
    const char* desc;
    uint8_t search[3];
    uint8_t replace[3];
    size_t len;
} StandardPatch;

typedef struct {
    const char* desc;
    uint8_t search[2];
    uint8_t replace[2];
    uint16_t vector_offset;
    uint8_t vector_data[3];
} VectorPatch;

StandardPatch std_patches[] = {
    {"DI IM 1 -> NOPs", {0xF3, 0xED, 0x56}, {0x00, 0x00, 0x00}, 3},
    {"IM 1 DI -> NOPs", {0xED, 0x56, 0xF3}, {0x00, 0x00, 0x00}, 3},
    {"VDP Ctrl Write",  {0xD3, 0xBF},       {0xD3, 0xA1},       2},
    {"VDP Stat Read",   {0xDB, 0xBF},       {0xDB, 0xA1},       2},
    {"VDP Data Write",  {0xD3, 0xBE},       {0xD3, 0xA0},       2},
    {"VDP Data Read",   {0xDB, 0xBE},       {0xDB, 0xA0},       2},
    {"LD C, 0xBE init", {0x0E, 0xBE},       {0x0E, 0xA0},       2}, 
    {"LD C, 0xBF init", {0x0E, 0xBF},       {0x0E, 0xA1},       2}
};

VectorPatch vec_patches[] = {
    {"Sound (RST 0x30)",      {0xD3, 0xFF}, {0xF7, 0x00}, 0x001B, {0xC3, 0x0C, 0x21}},
    {"Sound Alt (RST 0x30)",  {0xD3, 0xE0}, {0xF7, 0x00}, 0x001B, {0xC3, 0x0C, 0x21}},
    {"Joy/KP 1 (RST 0x18)",   {0xDB, 0xFF}, {0xDF, 0x00}, 0x0012, {0xC3, 0x06, 0x21}},
    {"Joy/KP 2 (RST 0x20)",   {0xDB, 0xFC}, {0xE7, 0x00}, 0x0015, {0xC3, 0x09, 0x21}},
    {"Joy Switch (RST 0x28)", {0xD3, 0xC0}, {0xEF, 0x00}, 0x0018, {0xC3, 0x03, 0x21}},
    {"KP Switch (RST 0x10)",  {0xD3, 0x80}, {0xD7, 0x00}, 0x000F, {0xC3, 0x00, 0x21}}
};

uint32_t parse_hex(const char* s) {
    if (!s) return 0;
    while (*s && !isxdigit((unsigned char)*s)) s++;
    return (uint32_t)strtoul(s, NULL, 16);
}

uint16_t calculate_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else crc <<= 1;
        }
    }
    return crc & 0xFFFF;
}

// Coleco CPU vectors -> cart jump slots (BIOS RST n / NMI jump through these).
static int rst_slot(int rst) {
    switch (rst) {
        case 0x08: return 0x0C; case 0x10: return 0x0F; case 0x18: return 0x12; case 0x20: return 0x15;
        case 0x28: return 0x18; case 0x30: return 0x1B; case 0x38: return 0x1E;
    }
    return -1;
}

static uint8_t rom_at(const uint8_t* rom, size_t n, size_t i) { return i < n ? rom[i] : 0; }

// Length of the Z80 instruction at rom[i].
static int z80_len(const uint8_t* rom, size_t n, size_t i) {
    uint8_t op = rom_at(rom, n, i);
    if (op == 0xCB) return 2;
    if (op == 0xED) return (rom_at(rom, n, i + 1) & 0xC7) == 0x43 ? 4 : 2;   // LD (nn),rr / LD rr,(nn)
    if (op == 0xDD || op == 0xFD) {
        uint8_t op2 = rom_at(rom, n, i + 1);
        if (op2 == 0xCB) return 4;
        if (op2 == 0xDD || op2 == 0xFD || op2 == 0xED) return 1;
        int disp = op2 == 0x34 || op2 == 0x35 || op2 == 0x36 ||
                   ((op2 & 0xC7) == 0x46 && op2 != 0x76) || (op2 >= 0x70 && op2 <= 0x77 && op2 != 0x76) ||
                   (op2 & 0xC7) == 0x86;
        return 1 + z80_len(rom, n, i + 1) + disp;
    }
    if ((op & 0xCF) == 0x01 || op == 0x22 || op == 0x2A || op == 0x32 || op == 0x3A ||
        (op & 0xC7) == 0xC2 || op == 0xC3 || (op & 0xC7) == 0xC4 || op == 0xCD) return 3;
    if ((op & 0xC7) == 0x06 || op == 0x10 || op == 0x18 || (op & 0xE7) == 0x20 ||
        (op & 0xC7) == 0xC6 || op == 0xD3 || op == 0xDB) return 2;
    return 1;
}

// Result of tracing the cart's code:
//   start    - 1 where a traced instruction starts
//   owner    - start offset of the traced instruction covering each byte, or -1
//   conflict - 1 where two paths decode the bytes differently (one is really data)
//   xstart/xconflict - the same, plus code reached only through jump variables (LD HL,nn /
//              LD (mm),HL where elsewhere the code does LD HL,(mm) and jumps to it). Less certain,
//              so only used to allow DI/IM 1 removal, never to block other patches.
typedef struct {
    size_t n;
    uint8_t *start, *conflict, *xstart, *xconflict;
    int32_t *owner, *xowner;
    int nmi_uses_ei, nmi_reads_iff;
} CodeMap;

typedef struct { uint32_t target; uint16_t var; } Store;
typedef struct {
    Store* stores; size_t nstores, capstores;
    uint8_t jumps[0x10000];                            // 1 = address used as a jump variable
} TraceVars;

typedef struct { int32_t* v; size_t len, cap; } Stack;
static void push(Stack* s, int32_t v) {
    if (s->len == s->cap) { s->cap = s->cap ? s->cap * 2 : 256; s->v = realloc(s->v, s->cap * sizeof *s->v); }
    s->v[s->len++] = v;
}

static void run_trace(const uint8_t* rom, size_t n, Stack* todo, uint8_t* start, int32_t* owner,
                      uint8_t* conflict, TraceVars* vars) {
    while (todo->len) {
        int32_t pc = todo->v[--todo->len];
        while (pc >= 0 && (size_t)pc < n && !start[pc]) {
            int len = z80_len(rom, n, pc);
            if ((size_t)pc + len > n) break;
            int clash = 0;
            for (int k = 0; k < len; k++) if (owner[pc + k] != -1) clash = 1;
            if (clash) {
                for (int k = 0; k < len; k++) {
                    int32_t o = owner[pc + k];
                    conflict[pc + k] = 1;
                    if (o != -1) for (int j = 0, ol = z80_len(rom, n, o); j < ol && (size_t)(o + j) < n; j++) conflict[o + j] = 1;
                }
                break;
            }
            start[pc] = 1;
            for (int k = 0; k < len; k++) owner[pc + k] = pc;
            uint8_t op = rom[pc];
            uint16_t nn = rom_at(rom, n, pc + 1) | (rom_at(rom, n, pc + 2) << 8);
            int32_t rel = pc + 0x8000 + 2 + (int8_t)rom_at(rom, n, pc + 1);
            int stop = 0;
            #define ADDR(a) do { if ((a) >= 0x8000 && (size_t)((a) - 0x8000) < n) push(todo, (a) - 0x8000); } while (0)
            if (op == 0xC3) { ADDR(nn); stop = 1; }
            else if ((op & 0xC7) == 0xC2 || op == 0xCD || (op & 0xC7) == 0xC4) ADDR(nn);
            else if (op == 0x18) { ADDR(rel); stop = 1; }
            else if (op == 0x10 || (op & 0xE7) == 0x20) ADDR(rel);
            else if ((op & 0xC7) == 0xC7) { if (op == 0xC7) stop = 1; else push(todo, rst_slot(op & 0x38)); }
            else if (op == 0xC9 || op == 0xE9) stop = 1;
            else if (op == 0xED && (rom_at(rom, n, pc + 1) == 0x45 || rom_at(rom, n, pc + 1) == 0x4D)) stop = 1;
            else if ((op == 0xDD || op == 0xFD) && rom_at(rom, n, pc + 1) == 0xE9) stop = 1;
            else if (op == 0x21 && rom_at(rom, n, pc + 3) == 0x22) {           // LD HL,nn / LD (mm),HL
                if (nn >= 0x8000 && (size_t)(nn - 0x8000) < n) {
                    if (vars->nstores == vars->capstores) {
                        vars->capstores = vars->capstores ? vars->capstores * 2 : 64;
                        vars->stores = realloc(vars->stores, vars->capstores * sizeof *vars->stores);
                    }
                    vars->stores[vars->nstores].target = nn - 0x8000;
                    vars->stores[vars->nstores].var = rom_at(rom, n, pc + 4) | (rom_at(rom, n, pc + 5) << 8);
                    vars->nstores++;
                }
            }
            else if (op == 0x2A) {                                              // LD HL,(mm) then jump to it
                size_t a = pc + 3;
                uint16_t b = rom_at(rom, n, a + 1) | (rom_at(rom, n, a + 2) << 8);
                uint8_t x = rom_at(rom, n, a), y = rom_at(rom, n, a + 1);
                if (x == 0xE9 || (x == 0xE3 && y == 0xC9) || (x == 0xE5 && y == 0xC9) ||
                    (x == 0xCD && b >= 0x8000 && (size_t)(b - 0x8000) < n && rom[b - 0x8000] == 0xE9)) vars->jumps[nn] = 1;
            }
            #undef ADDR
            if (stop) break;
            pc += len;
        }
    }
}

// True if the instruction op (op2 >= 0: two-byte op, op op2) can be reached from rom[i]
// (following branches and calls, not returns).
static int reaches_op(const uint8_t* rom, size_t n, size_t i, int op1, int op2) {
    uint8_t* seen = calloc(n, 1);
    Stack todo = {0};
    int budget = 4000, found = 0;
    push(&todo, (int32_t)i);
    #define PUSHA(a) do { if ((a) >= 0x8000 && (size_t)((a) - 0x8000) < n) push(&todo, (a) - 0x8000); } while (0)
    while (todo.len && budget > 0 && !found) {
        int32_t pc = todo.v[--todo.len];
        while (pc >= 0 && (size_t)pc < n && !seen[pc] && budget-- > 0) {
            seen[pc] = 1;
            uint8_t op = rom[pc];
            uint16_t nn = rom_at(rom, n, pc + 1) | (rom_at(rom, n, pc + 2) << 8);
            int32_t rel = pc + 0x8000 + 2 + (int8_t)rom_at(rom, n, pc + 1);
            if (op == op1 && (op2 < 0 || rom_at(rom, n, pc + 1) == op2)) { found = 1; break; }
            if (op == 0xC3) { PUSHA(nn); break; }
            if (op == 0x18) { PUSHA(rel); break; }
            if ((op & 0xC7) == 0xC2 || op == 0xCD || (op & 0xC7) == 0xC4) PUSHA(nn);
            else if (op == 0x10 || (op & 0xE7) == 0x20) PUSHA(rel);
            else if ((op & 0xC7) == 0xC7 && op != 0xC7) PUSHA(0x8000 + rst_slot(op & 0x38));
            if (op == 0xC9 || op == 0xE9 || op == 0xC7) break;
            if (op == 0xED && (rom_at(rom, n, pc + 1) == 0x45 || rom_at(rom, n, pc + 1) == 0x4D)) break;
            if ((op == 0xDD || op == 0xFD) && rom_at(rom, n, pc + 1) == 0xE9) break;
            pc += z80_len(rom, n, pc);
        }
    }
    #undef PUSHA
    free(todo.v); free(seen);
    return found;
}

// True if an instruction that uses C as a port (IN r,(C), OUT (C),r, INI/OUTI/INIR/OTIR...)
// starts at rom[i], after at most two DI/EI/NOPs. Unlike uses_c_as_port this needs no tracing, so
// it is kept this tight: such a sequence right after LD BC,nn is very unlikely in data.
static int port_op_soon(const uint8_t* rom, size_t n, size_t i) {
    for (int k = 0; k < 3 && i + 1 < n; k++, i++) {
        uint8_t op = rom[i], op2 = rom[i + 1];
        if (op == 0xED && ((op2 & 0xC6) == 0x40 || op2 == 0xA2 || op2 == 0xA3 || op2 == 0xAA || op2 == 0xAB ||
                           op2 == 0xB2 || op2 == 0xB3 || op2 == 0xBA || op2 == 0xBB)) return 1;
        if (op != 0xF3 && op != 0xFB && op != 0x00) return 0;
    }
    return 0;
}

// True if, following the code from rom[i], register C is used as an I/O port (IN r,(C), OUT (C),r,
// INI/OUTI/INIR/OTIR...) before anything changes it. Follows branches and calls.
static int uses_c_as_port(const uint8_t* rom, size_t n, size_t i) {
    uint8_t* seen = calloc(n, 1);
    Stack todo = {0};
    int budget = 2000, found = 0;
    push(&todo, (int32_t)i);
    #define PUSHA(a) do { if ((a) >= 0x8000 && (size_t)((a) - 0x8000) < n) push(&todo, (a) - 0x8000); } while (0)
    while (todo.len && budget > 0 && !found) {
        int32_t pc = todo.v[--todo.len];
        while (pc >= 0 && (size_t)pc < n && !seen[pc] && budget-- > 0) {
            seen[pc] = 1;
            uint8_t op = rom[pc], op2 = rom_at(rom, n, pc + 1);
            uint16_t nn = op2 | (rom_at(rom, n, pc + 2) << 8);
            int32_t rel = pc + 0x8000 + 2 + (int8_t)op2;
            if (op == 0xED) {
                if ((op2 & 0xC6) == 0x40 || op2 == 0xA2 || op2 == 0xA3 || op2 == 0xAA || op2 == 0xAB ||
                    op2 == 0xB2 || op2 == 0xB3 || op2 == 0xBA || op2 == 0xBB) { found = 1; break; }
                if (op2 == 0x4B) break;                                   // LD BC,(nn)
            }
            // anything that changes C ends this path
            if (op == 0x0E || op == 0x01 || op == 0xC1 || (op >= 0x48 && op <= 0x4F) || op == 0x0C || op == 0x0D ||
                op == 0x03 || op == 0x0B || op == 0xD9 || (op == 0xCB && (op2 & 7) == 1 && (op2 < 0x40 || op2 >= 0x80))) break;
            if (op == 0xC3) { PUSHA(nn); break; }
            if (op == 0x18) { PUSHA(rel); break; }
            if ((op & 0xC7) == 0xC2 || op == 0xCD || (op & 0xC7) == 0xC4) PUSHA(nn);
            else if (op == 0x10 || (op & 0xE7) == 0x20) PUSHA(rel);
            else if ((op & 0xC7) == 0xC7 && op != 0xC7) PUSHA(0x8000 + rst_slot(op & 0x38));
            if (op == 0xC9 || op == 0xE9 || op == 0xC7) break;
            if (op == 0xED && (op2 == 0x45 || op2 == 0x4D)) break;
            if ((op == 0xDD || op == 0xFD) && op2 == 0xE9) break;
            pc += z80_len(rom, n, pc);
        }
    }
    #undef PUSHA
    free(todo.v); free(seen);
    return found;
}

// True if disassembling from most of the 16 bytes before rom[i] lands on i (see trace_code).
static int lands_on(const uint8_t* rom, size_t n, size_t i) {
    int over = 0, land = 0;
    for (size_t s = i >= 16 ? i - 16 : 0; s < i; s++) {
        size_t pc = s;
        while (pc < i) pc += z80_len(rom, n, pc);
        if (pc == i) land++; else over++;
    }
    return land > over;
}

// Recursive-descent trace of cart code from the header entry points.
static CodeMap* trace_code(const uint8_t* rom, size_t n) {
    CodeMap* c = calloc(1, sizeof *c);
    c->n = n;
    c->start = calloc(n, 1); c->conflict = calloc(n, 1);
    c->owner = malloc(n * sizeof *c->owner);
    for (size_t i = 0; i < n; i++) c->owner[i] = -1;
    TraceVars* vars = calloc(1, sizeof *vars);
    Stack todo = {0};
    if (n >= 0x24 && ((rom[0] == 0xAA && rom[1] == 0x55) || (rom[0] == 0x55 && rom[1] == 0xAA))) {
        uint16_t entry = rom[0x0A] | (rom[0x0B] << 8);
        if (entry >= 0x8000 && (size_t)(entry - 0x8000) < n) push(&todo, entry - 0x8000);
        static const int slots[] = { 0x0C, 0x0F, 0x12, 0x15, 0x18, 0x1B, 0x1E, 0x21 };  // RSTs + NMI
        for (int k = 0; k < 8; k++) push(&todo, slots[k]);
    }
    run_trace(rom, n, &todo, c->start, c->owner, c->conflict, vars);

    c->xstart = malloc(n); memcpy(c->xstart, c->start, n);
    c->xconflict = malloc(n); memcpy(c->xconflict, c->conflict, n);
    c->xowner = malloc(n * sizeof *c->xowner); memcpy(c->xowner, c->owner, n * sizeof *c->owner);
    for (size_t used = 0;;) {
        size_t seeds = 0;
        for (size_t k = 0; k < vars->nstores; k++)
            if (vars->jumps[vars->stores[k].var]) { push(&todo, vars->stores[k].target); seeds++; }
        if (seeds == used) { todo.len = 0; break; }
        used = seeds;
        run_trace(rom, n, &todo, c->xstart, c->xowner, c->xconflict, vars);
    }
    // Routines called from 5 or more CALL nn in the ROM, traced or not: code the trace may not reach
    // when the game dispatches through tables (Venture: its VDP-access helper $98C9, 21 calls,
    // starts with a DI it never undoes). A CD byte counts as a call only where disassembling from
    // most of the 16 bytes before it lands on it (in data it is usually inside something else).
    // Only seeds xstart. Seeds go in order of first occurrence, like the web converter.
    uint16_t* calls = calloc(n, sizeof *calls);
    int32_t* order = malloc(n * sizeof *order);
    size_t norder = 0;
    for (size_t i = 0; i + 2 < n; i++)
        if (rom[i] == 0xCD && lands_on(rom, n, i)) {
            uint16_t a = rom[i + 1] | (rom[i + 2] << 8);
            if (a >= 0x8000 && (size_t)(a - 0x8000) < n) {
                if (!calls[a - 0x8000]) order[norder++] = a - 0x8000;
                if (calls[a - 0x8000] < 0xFFFF) calls[a - 0x8000]++;
            }
        }
    for (size_t k = 0; k < norder; k++) if (calls[order[k]] >= 5) push(&todo, order[k]);
    run_trace(rom, n, &todo, c->xstart, c->xowner, c->xconflict, vars);
    todo.len = 0;
    free(calls); free(order);
    c->nmi_uses_ei = n > 0x24 && reaches_op(rom, n, 0x21, 0xFB, -1);
    // LD A,I / LD A,R: the handler reads IFF2 (whether the game had interrupts enabled)
    c->nmi_reads_iff = n > 0x24 && (reaches_op(rom, n, 0x21, 0xED, 0x57) || reaches_op(rom, n, 0x21, 0xED, 0x5F));
    free(todo.v); free(vars->stores); free(vars);
    return c;
}

static void free_code(CodeMap* c) {
    free(c->start); free(c->conflict); free(c->xstart); free(c->xconflict);
    free(c->owner); free(c->xowner); free(c);
}

enum { ACCEPT_ANY, ACCEPT_IM1, ACCEPT_DI, ACCEPT_PORT, ACCEPT_LDC, ACCEPT_SOUND };

// ROM offsets (by ROM CRC16) that look like port I/O but are data the tracer can't tell apart, so they
// must not be patched.
typedef struct { uint16_t crc; int offsets[4]; } KnownData;
static const KnownData known_data[] = {
    {0xFCEB, {0x234D, 0x236D, -1}},   // Centipede (Atarisoft): "D3 C0" inside the shot sound's data, not OUT (C0),A
    {0x0393, {0x1B56, -1}},           // Blockem Sockem (2025-03-09): "D3 BF" inside LD HL,$BFD3 (score popup digits), untraced routine
};
static const int* cur_known = NULL;   // known data offsets for the ROM being converted (-1 terminated)

static int is_known_data(size_t i) {
    for (const int* p = cur_known; p && *p >= 0; p++) if ((size_t)*p == i) return 1;
    return 0;
}

// True if the DI at rom[i] starts a short critical section: an EI follows within a few
// straight-line instructions (e.g. Blockem Sockem's DI / OUT (C),E / OUT (C),D / EI around a VDP
// address). On the NABU that DI keeps the loader's status read (which resets the VDP address
// latch) from landing between the two address bytes, so it stays. A DI that waits for the NMI
// reaches a jump or HALT first.
static int di_guards_section(const uint8_t* rom, size_t n, size_t i) {
    size_t pc = i + 1;
    for (int k = 0; k < 8 && pc < n; k++) {
        uint8_t op = rom[pc], op2 = pc + 1 < n ? rom[pc + 1] : 0;
        if (op == 0xFB) return 1;
        if (op == 0xF3 || op == 0x76 || op == 0xC3 || op == 0xC9 || op == 0xE9 || op == 0x18 || op == 0x10 ||
            (op & 0xE7) == 0x20 || (op & 0xC7) == 0xC2 || (op & 0xC7) == 0xC0 || (op & 0xC7) == 0xC7 ||
            (op == 0xED && (op2 == 0x45 || op2 == 0x4D)) || ((op == 0xDD || op == 0xFD) && op2 == 0xE9)) return 0;
        pc += z80_len(rom, n, pc);
    }
    return 0;
}

// Offsets of the patches made by the last find_and_replace() call (for RST dispatchers).
static int last_sites[1024];
static int last_nsites;

// code: trace_code() result (optional). Matches that start inside a traced instruction are
// skipped. accept: ACCEPT_IM1 / ACCEPT_DI only patch traced code (and skip silently).
int find_and_replace(uint8_t* data, size_t size, const uint8_t* search, const uint8_t* replace, size_t len, const char* desc,
                     const CodeMap* code, int accept) {
    last_nsites = 0;
    int count = 0;
    for (size_t i = 0; i + len <= size; i++) {
        if (memcmp(&data[i], search, len) != 0) continue;
        if (code) {
            if (is_known_data(i)) { printf("\n    [0x%04X] %s skipped (known data)", (uint32_t)i, desc); continue; }
            int32_t own = code->owner[i];
            if (own != -1 && (size_t)own != i && !code->conflict[i]) {
                if (accept == ACCEPT_ANY) printf("\n    [0x%04X] %s skipped (inside instruction at 0x%04X)", (uint32_t)i, desc, (uint32_t)own);
                continue;
            }
            if (accept == ACCEPT_LDC) {
                // 0E BE / 0E BF also turn up in data (e.g. Gyruss's pointer table): patch traced code,
                // a "LD C,port / RET" helper, or where C is visibly used as a port
                int traced = code->xstart[i] && !code->xconflict[i];
                if (!traced && !(i + 2 < size && data[i + 2] == 0xC9) && !uses_c_as_port(data, size, i + 2)) {
                    printf("\n    [0x%04X] %s skipped (looks like data)", (uint32_t)i, desc);
                    continue;
                }
            }
            else if (accept == ACCEPT_PORT) {
                // both forms are 3 bytes; code the tracer missed counts when a C-port instruction
                // follows right away (GhostBlaster: LD BC,$10BE / DI / INIR in an untraced routine)
                int traced = code->xstart[i] && !code->xconflict[i];
                if (!(traced && uses_c_as_port(data, size, i + 3)) && !port_op_soon(data, size, i + 3)) continue;
            }
            else if (accept == ACCEPT_SOUND) {
                int traced = code->xstart[i] && !code->xconflict[i];
                if (!traced && !(i >= 2 && (data[i - 2] == 0x3E || data[i - 2] == 0xF6) && data[i - 1] >= 0x80 &&
                                 lands_on(data, size, i))) continue;
            }
            else if (accept != ACCEPT_ANY && (!code->xstart[i] || code->xconflict[i])) continue;
            if (accept == ACCEPT_DI && (code->nmi_uses_ei || di_guards_section(data, size, i))) continue;
        }
        printf("\n    [0x%04X] %s", (uint32_t)i, desc);
        memcpy(&data[i], replace, len);
        if (last_nsites < 1024) last_sites[last_nsites++] = (int)i;
        count++;
    }
    return count;
}

// The Coleco VDP interrupt is an NMI, which DI and IM cannot block or redirect. On the NABU it is a
// maskable IM 2 interrupt, so a cart's own IM 1, or a DI (Coleco games often DI and then wait for
// the NMI), stops vblank (and keyboard) handling. Carts whose NMI handler does EI were written for
// maskable interrupts (MSX ports) and use DI/EI deliberately, so their DIs stay. Single-byte DI
// can't be matched blindly without hitting data, so these only apply to traced code.
static const uint8_t im1_op[2] = {0xED, 0x56}, di_op[1] = {0xF3}, nops[2] = {0x00, 0x00};

// The loader stub's info block (loader/loader.z80), the 17 bytes before the ROM image: "C2NI",
// version 2, then its own runtime address, the free space for per-ROM code (next .. end), the
// loader's saved VDP status byte, its options byte and its IN A,(C) replacement.
// option_bits: loader options for this ROM, set in the stub copy by process_file.
#define STUB_INFO_LEN 17
#define LOADER_OPT_SWAP_FIRE 0x01       // loader_options bits (loader/interrupts.z80)
#define LOADER_OPT_SWAP_PORTS 0x02
#define LOADER_OPT_UP_BUTTON2 0x04
typedef struct { uint8_t* stub; int info_off, self, next, end, saved, options, read_port_c, option_bits; } Gap;

static int stub_info(Gap* g, uint8_t* stub, size_t stub_off) {
    int o = (int)stub_off - STUB_INFO_LEN;
    if (o < 0 || memcmp(&stub[o], "C2NI", 4) || stub[o + 4] != 2) return 0;
    #define W(k) (stub[o + (k)] | (stub[o + (k) + 1] << 8))
    g->stub = stub; g->info_off = o; g->self = W(5);
    g->next = W(7); g->end = W(9); g->saved = W(11); g->options = W(13); g->read_port_c = W(15);
    g->option_bits = 0;
    #undef W
    return 1;
}

// File offset in the stub of a loader runtime address.
static int gap_off(const Gap* g, int addr) { return addr - g->self + g->info_off; }

// RST dispatcher for a cart vector slot that the game uses itself: RSTs from the converter's patch
// sites (return address = site + 1) go to the loader routine, all others to the game's own target.
// Keeps AF and HL. Returns its length.
static int build_dispatcher(uint8_t* c, int addr, const int* sites, int nsites, int handler, int original) {
    int n = 0, fix[1024], nfix = 0;
    c[n++] = 0xE3; c[n++] = 0xF5;                               // EX (SP),HL / PUSH AF ; HL = return address
    for (int k = 0; k < nsites; k++) {
        int ret = 0x8000 + sites[k] + 1;
        c[n++] = 0x7D; c[n++] = 0xFE; c[n++] = ret & 0xFF; c[n++] = 0x20; c[n++] = 0x06;   // LD A,L / CP lo / JR NZ,next
        c[n++] = 0x7C; c[n++] = 0xFE; c[n++] = ret >> 8; c[n++] = 0xCA; fix[nfix++] = n; c[n++] = 0; c[n++] = 0;  // LD A,H / CP hi / JP Z,ours
    }
    c[n++] = 0xF1; c[n++] = 0xE3; c[n++] = 0xC3; c[n++] = original & 0xFF; c[n++] = original >> 8;  // POP AF / EX (SP),HL / JP original
    int ours = addr + n;
    for (int k = 0; k < nfix; k++) { c[fix[k]] = ours & 0xFF; c[fix[k] + 1] = ours >> 8; }
    c[n++] = 0xF1; c[n++] = 0xE3; c[n++] = 0xC3; c[n++] = handler & 0xFF; c[n++] = handler >> 8;   // ours: POP AF / EX (SP),HL / JP handler
    return n;
}

typedef struct { int slot; uint8_t data[3]; const char* desc; int sites[1024]; int nsites; } Vector;

// Point cart vector slots at the loader's routines. If the game's own slot already holds a JP (it
// uses that RST itself, e.g. Gyruss's RST $18 = write VDP register), install a dispatcher instead
// of overwriting it.
static void install_vectors(uint8_t* rom, size_t rom_len, const uint8_t* orig, Vector* v, int nv, Gap* gap) {
    static uint8_t buf[16 + 11 * 1024];
    for (int i = 0; i < nv; i++) {
        int slot = v[i].slot;
        if (rom_len <= (size_t)slot + 3) continue;
        int target = orig[slot + 1] | (orig[slot + 2] << 8);
        if (orig[slot] == 0xC3 && target != 0 && gap && gap->next) {
            int handler = v[i].data[1] | (v[i].data[2] << 8);
            int len = build_dispatcher(buf, gap->next, v[i].sites, v[i].nsites, handler, target);
            if (gap->next + len > gap->end) { printf("\n    [0x%04X] Vector Table: %s - no room for a dispatcher!", slot, v[i].desc); continue; }
            memcpy(&gap->stub[gap_off(gap, gap->next)], buf, len);
            printf("\n    [0x%04X] Vector Table: %s (game uses this RST too: dispatcher at 0x%04X)", slot, v[i].desc, gap->next);
            rom[slot] = 0xC3; rom[slot + 1] = gap->next & 0xFF; rom[slot + 2] = gap->next >> 8;
            gap->next += len;
        } else {
            printf("\n    [0x%04X] Vector Table: %s", slot, v[i].desc);
            memcpy(&rom[slot], v[i].data, 3);
        }
    }
}

// Sound written with C = port: OUT (C),r / OUTI / OUTD / OTIR / OTDR after a traced LD C,n with n a
// sound port ($E0-$FF) (Wonder Boy mutes the chip with LD C,$F0 / OUT (C),L / OUT (C),H and OTIR).
// Follows the code from each such LD C,n until C changes. Marks found[offset] = second opcode byte.
static int is_out_c_op(uint8_t op2) {
    return op2 == 0x41 || op2 == 0x49 || op2 == 0x51 || op2 == 0x59 || op2 == 0x61 || op2 == 0x69 || op2 == 0x79 ||
           op2 == 0xA3 || op2 == 0xAB || op2 == 0xB3 || op2 == 0xBB;
}

static void sound_via_c(const uint8_t* rom, size_t n, const CodeMap* code, uint8_t* found) {
    uint8_t* seen = malloc(n);
    Stack todo = {0};
    #define PUSHA(a) do { if ((a) >= 0x8000 && (size_t)((a) - 0x8000) < n) push(&todo, (a) - 0x8000); } while (0)
    for (size_t i = 0; i + 1 < n; i++) {
        if (rom[i] != 0x0E || rom[i + 1] < 0xE0 || !code->xstart[i] || code->xconflict[i]) continue;
        memset(seen, 0, n);
        todo.len = 0;
        push(&todo, (int32_t)i + 2);
        int budget = 2000;
        while (todo.len && budget > 0) {
            int32_t pc = todo.v[--todo.len];
            while (pc >= 0 && (size_t)pc < n && !seen[pc] && budget-- > 0) {
                seen[pc] = 1;
                uint8_t op = rom[pc], op2 = rom_at(rom, n, pc + 1);
                uint16_t nn = op2 | (rom_at(rom, n, pc + 2) << 8);
                int32_t rel = pc + 0x8000 + 2 + (int8_t)op2;
                if (op == 0xED && is_out_c_op(op2)) found[pc] = op2;
                if (op == 0xED && op2 == 0x4B) break;                         // LD BC,(nn)
                // anything that changes C ends this path
                if (op == 0x0E || op == 0x01 || op == 0xC1 || (op >= 0x48 && op <= 0x4F) || op == 0x0C || op == 0x0D ||
                    op == 0x03 || op == 0x0B || op == 0xD9 || (op == 0xCB && (op2 & 7) == 1 && (op2 < 0x40 || op2 >= 0x80))) break;
                if (op == 0xC3) { PUSHA(nn); break; }
                if (op == 0x18) { PUSHA(rel); break; }
                if ((op & 0xC7) == 0xC2 || op == 0xCD || (op & 0xC7) == 0xC4) PUSHA(nn);
                else if (op == 0x10 || (op & 0xE7) == 0x20) PUSHA(rel);
                if (op == 0xC9 || op == 0xE9 || (op & 0xC7) == 0xC7) break;
                if (op == 0xED && (op2 == 0x45 || op2 == 0x4D)) break;
                if ((op == 0xDD || op == 0xFD) && op2 == 0xE9) break;
                pc += z80_len(rom, n, pc);
            }
        }
    }
    #undef PUSHA
    free(todo.v); free(seen);
}

// The converter's RST $30 handler when some sites write sound with C = port. Those sites are
// RST $30 / op2 (the instruction's second byte, never 0); plain sound sites are RST $30 / NOP. It
// reads the byte after the RST: 0 goes to the loader's sound routine; otherwise it returns past the
// byte, through the thunk for op2 (ops[k] -> thunks[k]). Keeps all registers. Returns its length.
static int sound_c_handler(uint8_t* c, int addr, const uint8_t* ops, const int* thunks, int nops, int sound) {
    int n = 0, tail0 = addr + 12 + 5 * nops;
    const uint8_t head[] = {0xE3, 0xF5, 0x7E, 0xB7, 0x28, (uint8_t)(5 * nops + 1), 0x23};   // EX (SP),HL / PUSH AF / LD A,(HL) / OR A / JR Z,plain / INC HL
    memcpy(c, head, sizeof head); n = sizeof head;
    for (int k = 0; k < nops; k++) {                                       // CP op2 / JP Z,tail k
        int t = tail0 + 5 * k;
        c[n++] = 0xFE; c[n++] = ops[k]; c[n++] = 0xCA; c[n++] = t & 0xFF; c[n++] = t >> 8;
    }
    c[n++] = 0xF1; c[n++] = 0xE3; c[n++] = 0xC3; c[n++] = sound & 0xFF; c[n++] = sound >> 8;       // plain: POP AF / EX (SP),HL / JP sound
    for (int k = 0; k < nops; k++) {                                       // tail k: POP AF / EX (SP),HL / JP thunk k
        c[n++] = 0xF1; c[n++] = 0xE3; c[n++] = 0xC3; c[n++] = thunks[k] & 0xFF; c[n++] = thunks[k] >> 8;
    }
    return n;
}

// Replacement for OUT (C),r / OUTI / OUTD / OTIR / OTDR (op2), reached through sound_c_handler: C = $E0-$FF goes to
// the loader's sound routine (keeps all registers), any other port is really written (a routine
// shared with the VDP). Registers and the B/HL/Z results as the real instruction. Returns its length.
static int sound_c_thunk(uint8_t* c, uint8_t op2, int sound) {
    int n = 0;
    c[n++] = 0xF5; c[n++] = 0x79; c[n++] = 0xFE; c[n++] = 0xE0; c[n++] = 0x38; c[n++] = 0;   // PUSH AF / LD A,C / CP $E0 / JR C,real
    if (op2 < 0xA0) {                                                      // OUT (C),r
        int r = (op2 >> 3) & 7;
        c[n++] = 0xF1; c[n++] = 0xF5;                                      // POP AF / PUSH AF
        if (r != 7) c[n++] = 0x78 | r;                                     // LD A,r
        c[n++] = 0xCD; c[n++] = sound & 0xFF; c[n++] = sound >> 8; c[n++] = 0xF1; c[n++] = 0xC9;   // CALL sound / POP AF / RET
    } else {                                                               // OUTI / OUTD / OTIR / OTDR
        int loop = n;
        c[n++] = 0x7E; c[n++] = 0xCD; c[n++] = sound & 0xFF; c[n++] = sound >> 8;   // loop: LD A,(HL) / CALL sound
        c[n++] = op2 & 0x08 ? 0x2B : 0x23; c[n++] = 0x05;                  // INC HL or DEC HL / DEC B
        if (op2 & 0x10) { c[n] = 0x20; c[n + 1] = (uint8_t)(loop - (n + 2)); n += 2; }   // JR NZ,loop
        c[n++] = 0xF1; c[n++] = 0x04; c[n++] = 0x05; c[n++] = 0xC9;        // POP AF / INC B / DEC B (Z = B is 0) / RET
    }
    c[5] = (uint8_t)(n - 6);
    c[n++] = 0xF1; c[n++] = 0xED; c[n++] = op2; c[n++] = 0xC9;             // real: POP AF / the instruction / RET
    return n;
}

// An NMI handler that does EI (MSX-style) re-enters itself on the NABU: the loader calls it before
// reading the VDP status, so the level-triggered interrupt is still pending when the handler's EI
// runs, and the game's frame timers run several times too fast (Fall Guy). Point the cart's NMI
// slot at a wrapper that reads the status first (clearing the interrupt), adds the bits to the
// loader's saved status for READ_REGISTER, and jumps to the game's handler with the stack and
// registers as they were.
// A handler that reads IFF2 with LD A,I (Team Pixelboy's Wonder Boy: defer the frame if the game
// had interrupts disabled) always sees "disabled" on the NABU, where taking the maskable interrupt
// clears IFF2, and never runs its frame. For those the wrapper also does EI before the jump (the
// interrupt was taken, so the game had interrupts enabled; the VDP interrupt is already cleared).
static void wrap_nmi(uint8_t* rom, size_t rom_len, Gap* gap, int reads_iff) {
    if (!gap || !gap->saved || rom_len < 0x24 || rom[0x21] != 0xC3) return;
    int target = rom[0x22] | (rom[0x23] << 8), s = gap->saved, n = 0;
    uint8_t c[16];
    const uint8_t head[] = {0xF5, 0xE5, 0xDB, 0xA1, 0x21, s & 0xFF, s >> 8, 0xB6, 0x77, 0xE1, 0xF1};   // PUSH AF / PUSH HL / IN A,($A1) / LD HL,saved / OR (HL) / LD (HL),A / POP HL / POP AF
    memcpy(c, head, sizeof head); n = sizeof head;
    if (reads_iff) c[n++] = 0xFB;                                           // EI
    c[n++] = 0xC3; c[n++] = target & 0xFF; c[n++] = target >> 8;            // JP handler
    if (gap->next + n > gap->end) { printf("\n    [0x0021] NMI wrapper: no room!"); return; }
    memcpy(&gap->stub[gap_off(gap, gap->next)], c, n);
    printf("\n    [0x0021] NMI handler %s: wrapper at 0x%04X clears the VDP interrupt first%s",
           reads_iff ? "reads IFF2" : "does EI", gap->next, reads_iff ? " and enables interrupts" : "");
    rom[0x22] = gap->next & 0xFF; rom[0x23] = gap->next >> 8;
    gap->next += n;
}

// VDP port loaded into C other than by LD C,n (e.g. CVBasic's LD BC,$80BE before OTIR). Only patched
// in traced code where C then really is used as a port, so 16-bit constants ending in $BE/$BF are safe.
StandardPatch smart_port_patches[] = {
    {"LD BC,nn (C=0xBE) port",  {0x01, 0xBE},       {0x01, 0xA0},       2},
    {"LD BC,nn (C=0xBF) port",  {0x01, 0xBF},       {0x01, 0xA1},       2},
    {"LD A,0xBE / LD C,A port", {0x3E, 0xBE, 0x4F}, {0x3E, 0xA0, 0x4F}, 3},
    {"LD A,0xBF / LD C,A port", {0x3E, 0xBF, 0x4F}, {0x3E, 0xA1, 0x4F}, 3},
};

void apply_auto_patches(uint8_t* rom, size_t rom_len, int swap_joy, int smart_ports, Gap* gap) {
    CodeMap* code = trace_code(rom, rom_len);
    uint8_t* orig = malloc(rom_len); memcpy(orig, rom, rom_len);

    printf("\n  Applying Standard Patches:");
    for (int i = 0; i < 8; i++) {
        int accept = std_patches[i].search[0] == 0x0E ? ACCEPT_LDC : ACCEPT_ANY;   // LD C,$BE / LD C,$BF
        find_and_replace(rom, rom_len, std_patches[i].search, std_patches[i].replace, std_patches[i].len, std_patches[i].desc, code, accept);
    }
    if (smart_ports) {
        for (int i = 0; i < 4; i++)
            find_and_replace(rom, rom_len, smart_port_patches[i].search, smart_port_patches[i].replace,
                             smart_port_patches[i].len, smart_port_patches[i].desc, code, ACCEPT_PORT);
    }

    printf("\n  Applying Interrupt Patches:");
    find_and_replace(rom, rom_len, im1_op, nops, 2, "IM 1 -> NOPs", code, ACCEPT_IM1);
    // Real BIOS $1987 is GAME_OPT after its screen clear (The Yolk's on You calls it directly). The
    // NABU loader's BIOS is a rewrite where $1987 is mid-instruction; its equivalent point is $197C.
    static const uint8_t opt_old[3] = {0xCD, 0x87, 0x19}, opt_new[3] = {0xCD, 0x7C, 0x19};
    find_and_replace(rom, rom_len, opt_old, opt_new, 3, "CALL $1987 (GAME_OPT text) -> $197C", code, ACCEPT_IM1);
    find_and_replace(rom, rom_len, di_op, nops, 1, "DI -> NOP", code, ACCEPT_DI);

    printf("\n  Applying Vector Patches:");
    static Vector vecs[8];
    int nv = 0, reads_fc = 0;
    for (int i = 0; i < 6; i++) {
        int found = find_and_replace(rom, rom_len, vec_patches[i].search, vec_patches[i].replace, 2, vec_patches[i].desc, code, ACCEPT_ANY);
        if (found > 0) {
            uint8_t v_data[3];
            memcpy(v_data, vec_patches[i].vector_data, 3);
            if (vec_patches[i].search[0] == 0xDB && vec_patches[i].search[1] == 0xFC) reads_fc = 1;
            int k = 0;
            while (k < nv && vecs[k].slot != vec_patches[i].vector_offset) k++;       // same slot: merge sites
            if (k == nv) { vecs[k].slot = vec_patches[i].vector_offset; memcpy(vecs[k].data, v_data, 3); vecs[k].desc = vec_patches[i].desc; vecs[k].nsites = 0; nv++; }
            for (int s = 0; s < last_nsites && vecs[k].nsites < 1024; s++) vecs[k].sites[vecs[k].nsites++] = last_sites[s];
        }
    }
    // The sound chip answers to every port $E0-$FF (Wonder Boy writes $F0). $FF and $E0 are patched
    // anywhere above; the others in traced code, or right after LD A,n / OR n with a sound latch byte
    // (bit 7 set; Wonder Boy's music driver is only reached through a jump table).
    const VectorPatch* snd = &vec_patches[0];
    int ks = 0;
    while (ks < nv && vecs[ks].slot != snd->vector_offset) ks++;
    for (int p = 0xE1; p < 0xFF; p++) {
        static char descs[0x20][32];
        char* desc = descs[p & 0x1F];
        snprintf(desc, sizeof descs[0], "Sound port 0x%X (RST 0x30)", p);
        const uint8_t out_p[2] = {0xD3, (uint8_t)p};
        if (find_and_replace(rom, rom_len, out_p, snd->replace, 2, desc, code, ACCEPT_SOUND) > 0) {
            if (ks == nv) { vecs[ks].slot = snd->vector_offset; memcpy(vecs[ks].data, snd->vector_data, 3); vecs[ks].desc = desc; vecs[ks].nsites = 0; nv++; }
            for (int s = 0; s < last_nsites && vecs[ks].nsites < 1024; s++) vecs[ks].sites[vecs[ks].nsites++] = last_sites[s];
        }
    }
    if (gap && gap->next) {
        uint8_t* found = calloc(rom_len, 1);
        sound_via_c(rom, rom_len, code, found);
        int sound = snd->vector_data[1] | (snd->vector_data[2] << 8), nops = 0, size = 0;
        uint8_t ops[16], bodies[16][32];
        int blen[16], thunks[16];
        for (size_t i = 0; i < rom_len; i++) {                               // one thunk per instruction, in order of first site
            if (!found[i]) continue;
            int k = 0;
            while (k < nops && ops[k] != found[i]) k++;
            if (k == nops) { ops[nops] = found[i]; blen[nops] = sound_c_thunk(bodies[nops], found[i], sound); size += blen[nops++]; }
        }
        size += 12 + 10 * nops;
        if (nops && gap->next + size > gap->end) printf("\n    Sound via OUT (C) - no room!");
        else if (nops) {
            static uint8_t h[256];
            for (int k = 0; k < nops; k++) {
                thunks[k] = gap->next;
                memcpy(&gap->stub[gap_off(gap, gap->next)], bodies[k], blen[k]);
                gap->next += blen[k];
            }
            int hlen = sound_c_handler(h, gap->next, ops, thunks, nops, sound);
            memcpy(&gap->stub[gap_off(gap, gap->next)], h, hlen);
            if (ks == nv) { vecs[ks].slot = snd->vector_offset; memcpy(vecs[ks].data, snd->vector_data, 3); vecs[ks].desc = "Sound (RST 0x30)"; vecs[ks].nsites = 0; nv++; }
            for (size_t i = 0; i < rom_len; i++) {
                if (!found[i]) continue;
                uint8_t op2 = found[i];
                if (op2 == 0xA3 || op2 == 0xAB || op2 == 0xB3 || op2 == 0xBB)
                    printf("\n    [0x%04X] Sound via %s (RST 0x30)", (uint32_t)i,
                           op2 == 0xA3 ? "OUTI" : op2 == 0xAB ? "OUTD" : op2 == 0xB3 ? "OTIR" : "OTDR");
                else printf("\n    [0x%04X] Sound via OUT (C),%c (RST 0x30)", (uint32_t)i, "BCDEHL?A"[(op2 >> 3) & 7]);
                rom[i] = 0xF7;                                              // RST $30 / op2
                if (vecs[ks].nsites < 1024) vecs[ks].sites[vecs[ks].nsites++] = (int)i;
            }
            static char sdesc[96];
            snprintf(sdesc, sizeof sdesc, "%s (OUT (C) sites: handler at 0x%04X)", vecs[ks].desc, gap->next);
            vecs[ks].desc = sdesc;
            vecs[ks].data[0] = 0xC3; vecs[ks].data[1] = gap->next & 0xFF; vecs[ks].data[2] = gap->next >> 8;
            gap->next += hlen;
        }
        free(found);
    }
    // Controllers read with IN A,(C), C = port (Wonder Boy): if traced code loads C with a controller
    // port (LD C,$FC / LD C,$FF), IN A,(C) -> RST $08 -> the loader's read_port_c, which reads a
    // controller for C = $E0-$FF and the real port otherwise.
    int via_c = 0, via_c_fc = 0;
    for (size_t i = 0; i + 1 < rom_len; i++)
        if (rom[i] == 0x0E && (rom[i + 1] == 0xFC || rom[i + 1] == 0xFF) && code->xstart[i] && !code->xconflict[i]) {
            via_c = 1;
            if (rom[i + 1] == 0xFC) via_c_fc = 1;
        }
    if (via_c && gap && gap->read_port_c) {
        static const uint8_t in_c[2] = {0xED, 0x78}, rst08[2] = {0xCF, 0x00};
        if (find_and_replace(rom, rom_len, in_c, rst08, 2, "IN A,(C) controller (RST 0x08)", code, ACCEPT_IM1) > 0) {
            Vector* v = &vecs[nv++];
            v->slot = rst_slot(0x08); v->desc = "IN A,(C) (RST 0x08)"; v->nsites = 0;
            v->data[0] = 0xC3; v->data[1] = gap->read_port_c & 0xFF; v->data[2] = gap->read_port_c >> 8;
            for (int s = 0; s < last_nsites && v->nsites < 1024; s++) v->sites[v->nsites++] = last_sites[s];
            if (via_c_fc) reads_fc = 1;
        }
    }
    // The loader reads NABU controller 1 for port FF and controller 2 for port FC. On a ColecoVision,
    // FC is controller 1, so a game that reads FC directly expects player 1 there: swap them
    // (auto = 2), unless forced on (1) / off (0). The loader does the swap (loader_options bit 1).
    int swap = swap_joy == 2 ? reads_fc : swap_joy;
    if (swap_joy == 2 && swap) printf("\n  Joystick ports swapped (auto: game reads controller 1 port FC)");
    if (swap && gap) gap->option_bits |= LOADER_OPT_SWAP_PORTS;
    install_vectors(rom, rom_len, orig, vecs, nv, gap);
    if (code->nmi_uses_ei || code->nmi_reads_iff) wrap_nmi(rom, rom_len, gap, code->nmi_reads_iff);
    free(orig);
    free_code(code);
}

// patches.z80: known titles' patches, by ROM CRC16, in the loader's patcher format (brijohn's
// patcher.z80). "patchset:" is a table of .word crc, .word label pairs; each label's directives
// assemble to a byte stream of entries: .word address (0 = end), .byte count, then count data bytes
// (written as .byte or .word, little-endian). Addresses are CPU addresses ($8000 = ROM offset 0).
// Returns 1 if the CRC is listed (its patches replace the automatic ones, even an empty list).
static char* patches_text = NULL;       // patches.z80, loaded by main (NULL: none)

static int directive(char* line, char** args) {         // 1 = .word, 2 = .byte, 0 = other; strips comments
    char* c = strchr(line, ';'); if (c) *c = 0;
    while (*line == ' ' || *line == '\t') line++;
    if (!strncmp(line, ".word", 5) && (line[5] == ' ' || line[5] == '\t')) { *args = line + 5; return 1; }
    if (!strncmp(line, ".byte", 5) && (line[5] == ' ' || line[5] == '\t')) { *args = line + 5; return 2; }
    return 0;
}

static int label_of(const char* line, char* out, size_t n) {   // "name:" at the start of a line
    size_t k = 0;
    while (line[k] && (isalnum((unsigned char)line[k]) || line[k] == '_') && k + 1 < n) { out[k] = line[k]; k++; }
    if (k == 0 || line[k] != ':' || isdigit((unsigned char)line[0])) return 0;
    out[k] = 0; return 1;
}

int apply_external_patches(uint8_t* rom, size_t rom_len, uint16_t target_crc) {
    if (!patches_text) return 0;
    char* text = strdup(patches_text);
    char cur[128] = "", target[128] = "";
    uint8_t* s = malloc(strlen(text) + 16);      // byte stream of the target's entries
    size_t sn = 0;
    int pending_crc = -1, found = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) { if (!found) break; free(text); text = strdup(patches_text); cur[0] = 0; }
        for (char* line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
            char name[128], *args;
            if (label_of(line, name, sizeof name)) { strcpy(cur, name); continue; }
            int d = directive(line, &args);
            if (!d) continue;
            // split args on commas by hand (strtok is busy with lines)
            char* p = args;
            while (*p) {
                while (*p == ' ' || *p == '\t' || *p == ',') p++;
                if (!*p) break;
                char tokbuf[128]; size_t k = 0;
                while (p[k] && p[k] != ',' && k + 1 < sizeof tokbuf) { tokbuf[k] = p[k]; k++; }
                p += k; tokbuf[k] = 0;
                while (k && (tokbuf[k - 1] == ' ' || tokbuf[k - 1] == '\t')) tokbuf[--k] = 0;
                int is_num = isdigit((unsigned char)tokbuf[0]);
                long v = is_num ? strtol(tokbuf, NULL, 0) : 0;
                if (pass == 0 && !strcmp(cur, "patchset") && d == 1) {
                    if (pending_crc < 0) { if (is_num) pending_crc = (int)v; }
                    else {
                        if (!found && pending_crc == target_crc && !is_num) { strcpy(target, tokbuf); found = 1; }
                        pending_crc = -1;
                    }
                } else if (pass == 1 && !strcmp(cur, target)) {
                    if (d == 2) s[sn++] = (uint8_t)v;
                    else { s[sn++] = (uint8_t)v; s[sn++] = (uint8_t)(v >> 8); }
                }
            }
        }
    }
    free(text);
    if (!found) { free(s); return 0; }
    printf("\n  [CRC MATCH]: %s", target);
    for (size_t p = 0; p + 2 < sn; ) {
        int addr = s[p] | (s[p + 1] << 8), n = s[p + 2];
        if (addr == 0) break;
        const uint8_t* data = &s[p + 3];
        int have = (int)(sn - (p + 3)) < n ? (int)(sn - (p + 3)) : n;
        p += 3 + n;
        long off = addr - 0x8000;
        if (off < 0 || (size_t)(off + have) > rom_len) { printf("\n    [0x%04X] outside the ROM, skipped", addr); continue; }
        memcpy(&rom[off], data, have);
        printf("\n    [0x%04X] %d byte%s:", (unsigned)off, have, have == 1 ? "" : "s");
        for (int k = 0; k < have; k++) printf(" %02x", data[k]);
    }
    free(s);
    return 1;
}

void process_file(const char* filename, uint8_t* stub_data, size_t stub_len, int skip_patches, int swap_joy, int smart_ports, int swapfire, int upbutton2) {
    FILE* f = fopen(filename, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); size_t rom_len = ftell(f); fseek(f, 0, SEEK_SET);
    if (rom_len > MAX_ROM_SIZE) { printf("Processing: %-32s OVER 32KB, SKIPPED\n", filename); fclose(f); return; }
    uint8_t* rom = (uint8_t*)malloc(rom_len);
    fread(rom, 1, rom_len, f); fclose(f);
    uint16_t crc = calculate_crc16(rom, rom_len);
    printf("Processing: %-32s (CRC: 0x%04X)", filename, crc);
    size_t stub_off = stub_len - MAX_ROM_SIZE;
    uint8_t* stub = (uint8_t*)malloc(stub_off);                    // per-ROM copy: RST dispatchers go in its gap
    memcpy(stub, stub_data, stub_off);
    Gap gap;
    int have_gap = stub_info(&gap, stub, stub_off);
    if (!have_gap) printf("\n  WARNING: loader stub has no info block (old stub?): no RST dispatchers, NMI wrapper or options");
    if (swapfire && have_gap) {
        stub[gap_off(&gap, gap.options)] |= LOADER_OPT_SWAP_FIRE;
        printf("\n  Fire buttons swapped: joystick fire = right button");
    }
    if (upbutton2 && have_gap) {
        stub[gap_off(&gap, gap.options)] |= LOADER_OPT_UP_BUTTON2;
        printf("\n  Up = button 2 (%s Coleco button)", swapfire ? "left" : "right");
    }
    cur_known = NULL;
    for (size_t k = 0; k < sizeof known_data / sizeof known_data[0]; k++)
        if (known_data[k].crc == crc) cur_known = known_data[k].offsets;
    if (!skip_patches && !apply_external_patches(rom, rom_len, crc))
        apply_auto_patches(rom, rom_len, swap_joy, smart_ports, have_gap ? &gap : NULL);
    if (have_gap) stub[gap_off(&gap, gap.options)] |= gap.option_bits;     // loader options (port swap)

    char base[1024]; strncpy(base, filename, 1023);
    char* dot = strrchr(base, '.'); if (dot) *dot = '\0';
    size_t pay_len = stub_off + rom_len;
    uint8_t* nabu_out = (uint8_t*)malloc(pay_len);
    if (nabu_out) {
        memcpy(nabu_out, stub, stub_off);
        memcpy(nabu_out + stub_off, rom, rom_len);
        char out[1100]; sprintf(out, "%s.nabu", base);
        FILE* nf = fopen(out, "wb");
        if (nf) { fwrite(nabu_out, 1, pay_len, nf); fclose(nf); printf("\n  [+] Created: %s", out); }
        
        uint8_t* com_out = (uint8_t*)malloc(STUB_SIZE + pay_len);
        if (com_out) {
            uint16_t se = 0x0100 + STUB_SIZE + (uint16_t)pay_len - 1;
            uint16_t de = 0x140D + (uint16_t)pay_len - 1;
            uint8_t cs[STUB_SIZE] = {0xF3, 0x01, pay_len&0xFF, pay_len>>8, 0x21, se&0xFF, se>>8, 0x11, de&0xFF, de>>8, 0xED, 0xB8, 0xC3, 0x10, 0x14};
            memcpy(com_out, cs, STUB_SIZE); memcpy(com_out + STUB_SIZE, nabu_out, pay_len);
            sprintf(out, "%s.com", base);
            FILE* cf = fopen(out, "wb");
            if (cf) { fwrite(com_out, 1, STUB_SIZE + pay_len, cf); fclose(cf); printf("\n  [+] Created: %s", out); }
            free(com_out);
        }
        printf("\n\n"); free(nabu_out);
    }
    free(stub);
    free(rom);
}

int main(int argc, char* argv[]) {
    printf("coleco2nabu v0.4 by GTAMP (c) 2026\n");
    printf("based on Coleco Loader by Brian Johnson\n");
    int skip = 0, swap = 2, f_count = 0, smart = 1, swapfire = 0, upbutton2 = 0;   // swap: 2 = auto
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-nopatch")) skip = 1;
        else if (!strcmp(argv[i], "-swapfire")) swapfire = 1;
        else if (!strcmp(argv[i], "-upbutton2")) upbutton2 = 1;
        else if (!strcmp(argv[i], "-swapjoy")) swap = 1;
        else if (!strcmp(argv[i], "-noswapjoy")) swap = 0;
        else if (!strcmp(argv[i], "-nosmart")) smart = 0;
        else if (!strcmp(argv[i], "-2")) ;   // old 2-player stub switch: the one stub does both now
        else f_count++;
    }
    if (f_count == 0) { printf("\nUsage: coleco2nabu <roms> [-nopatch] [-swapjoy|-noswapjoy] [-swapfire] [-upbutton2] [-nosmart]\n"); return 1; }

    FILE* pf = fopen("patches.z80", "rb");       // known titles' patches (optional)
    if (pf) {
        fseek(pf, 0, SEEK_END); long pl = ftell(pf); fseek(pf, 0, SEEK_SET);
        patches_text = calloc(pl + 1, 1); fread(patches_text, 1, pl, pf); fclose(pf);
    } else printf("Note: patches.z80 not found, known-title patches are not applied\n");

    FILE* ts = fopen("coleco2nabu.001", "rb");   // loader stub, built from loader/ (build.py)
    if (!ts) { ts = fopen("coleco2nabu.bin", "rb"); if (!ts) return 1; }
    fseek(ts, 0, SEEK_END); size_t sl = ftell(ts); fseek(ts, 0, SEEK_SET);
    uint8_t* sd = (uint8_t*)malloc(sl); fread(sd, 1, sl, ts); fclose(ts);

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        char* pat = argv[i]; struct _finddata_t cf; intptr_t h;
        char dir[512] = ""; 
        const char *ls = strrchr(pat, '\\'), *lf = strrchr(pat, '/'), *sep = (ls > lf) ? ls : lf;
        if (sep) { size_t len = (sep - pat) + 1; strncpy(dir, pat, len); dir[len] = '\0'; }
        if ((h = _findfirst(pat, &cf)) != -1L) {
            do { if (!(cf.attrib & _A_SUBDIR)) { 
                char full[2048]; snprintf(full, 2048, "%s%s", dir, cf.name); 
                process_file(full, sd, sl, skip, swap, smart, swapfire, upbutton2);
            } } while (_findnext(h, &cf) == 0);
            _findclose(h);
        }
    }
    free(sd); return 0;
}