/* NABU CP/M TNFS client v0.4 by GTAMP (c) 2026
   Trivial/Tiny/Taco Network File System
   https://github.com/gtampdotcom/NABU-PC-PROJECTS
   https://gtamp.com/nabu
   
   If this looks like AI generated code, that's because it is.
   
   MIT license
*/

#define BIN_TYPE          BIN_CPM
#define DISABLE_VDP
#define DISABLE_KEYBOARD_INT

#include <stdio.h>
#include <string.h>
#include <conio.h>
#include <cpm.h>
#include "../../NABULIB/NABU-LIB.h"
#include "../../NABULIB/RetroNET-FileStore.h"

#define PAGE_SIZE    20
#define MAX_SERVERS  20
#define MAX_FILES    20

/* ---------- TNFS command bytes ---------- */
#define CMD_MOUNT      0x00
#define CMD_OPENDIR    0x10
#define CMD_READDIR    0x11
#define CMD_CLOSEDIR   0x12
#define CMD_OPENDIRX   0x17
#define CMD_READDIRX   0x18
#define CMD_OPENFILE   0x20
#define CMD_READFILE   0x21
#define CMD_CLOSEFILE  0x23

/* OPENDIRX option flags (TNFS_DIROPT byte) */
#define DIROPT_DEFAULT          0x00  /* hides ".." - we inject our own manually */

/* OPENDIRX sort flags (TNFS_DIRSORT byte): 0x00 = default (dirs first, name A-Z) */
#define DIRSORT_DEFAULT         0x00

/* READDIRX per-entry flags */
#define DIRENTRY_DIR            0x01  /* entry is a directory */

/* READDIRX status flags */
#define DIRSTATUS_EOF           0x01  /* end of directory in this response */

/* TNFS error we check for */
#define TNFS_ENOSYS             0x16  /* function not implemented – fall back */

/* -----------------------------------------------------------------------
   ctype replacements – avoids pulling in 256-byte lookup tables
   ----------------------------------------------------------------------- */
static uint8_t my_isprint(uint8_t c) { return (c >= 0x20 && c < 0x7F); }
static uint8_t my_isspace(uint8_t c) { return (c == ' ' || c == '\t' || c == '\r' || c == '\n'); }
static uint8_t my_isdigit(uint8_t c) { return (c >= '0' && c <= '9'); }
static char    my_toupper(char c)    { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* Simple atoi replacement – avoids stdlib.h pull-in */
static uint8_t my_atoi8(const char *s) {
    uint8_t v = 0;
    while (my_isdigit((uint8_t)*s)) v = (uint8_t)(v * 10 + (*s++ - '0'));
    return v;
}

/* -----------------------------------------------------------------------
   Global state
   ----------------------------------------------------------------------- */
static uint8_t  g_isVT100 = 0;
static uint8_t  g_buf[1024];
static uint8_t  g_txBuf[1024];
static uint8_t  g_tcp  = 0xff;
static uint16_t g_sid  = 0x0000;
static uint8_t  g_seq  = 0x00;
static char     g_currentPath[128] = "/";

/* -----------------------------------------------------------------------
   File list: flat buffer + pointer array.
   ----------------------------------------------------------------------- */
typedef struct { char name[63]; uint8_t isDir; } FileEntry;   /* 64 bytes */

static FileEntry  g_fileStore[MAX_FILES];   /* 20 * 64 = 1280 bytes */
static FileEntry *g_fileList[MAX_FILES];    /* pointer array – no index multiply */
static uint8_t    g_fileCount = 0;          /* entries on current page (<=20) */
static uint8_t    g_dirPage   = 0;          /* current page number (0-based) */
static uint8_t    g_hasMore   = 0;          /* 1 if a next page exists */
static uint8_t    g_dirHandle = 0xff;       /* open dir handle, 0xff = closed */
static uint8_t    g_useX      = 2;          /* 2=unchecked 1=OPENDIRX works 0=legacy */
static uint8_t    g_rawKeys   = 1;          /* 1=raw hardware (default)  0=BIOS (/t) */

/* -----------------------------------------------------------------------
   Server list: flat buffer + pointer array
   ----------------------------------------------------------------------- */
#define SERVER_BUF_SIZE  (MAX_SERVERS * 64) 
static char   g_serverBuf[SERVER_BUF_SIZE];
static char  *g_servers[MAX_SERVERS]; 
static uint8_t g_numServers = 0;

/* -----------------------------------------------------------------------
   Terminal helpers
   ----------------------------------------------------------------------- */
void detect_terminal(void) {
    volatile uint8_t *ioBytePtr = (volatile uint8_t *)0x0003;
    uint8_t val = *ioBytePtr;
    g_isVT100 = (val == 0x01 || val == 0x93 || val == 0x94 || val == 0x9B || val == 0xFF) ? 1 : 0;
}

void clear_screen(void) {
    if (g_isVT100) printf("\x1B[H\x1B[2J");
    else putchar(0x1A);
}

void move_cursor(uint8_t r, uint8_t c) {
    if (g_isVT100) printf("\x1B[%d;%dH", r + 1, c + 1);
    else printf("\x1B\x3D%c%c", r + 32, c + 32);
}

void get_input(char *buffer, uint8_t maxLen) {
    uint8_t pos = 0, c;
    while (1) {
        c = (uint8_t)bios(3, 0, 0);
        if (c == 0x0D || c == 0x0A) break;
        if (c == 0x08 || c == 0x7F) {
            if (pos > 0) { pos--; printf("\x08 \x08"); }
        } else if (pos < (uint8_t)(maxLen - 1) && my_isprint(c)) {
            buffer[pos++] = (char)c; putchar(c);
        }
    }
    buffer[pos] = '\0'; printf("\n");
}

static uint8_t last_raw = 0;  /* raw mode: remembers last reported key */

uint8_t get_key(void) {
    if (g_rawKeys) {
        /* ---- Raw hardware path ---- */
        uint8_t raw, pressed;
        __asm__("di");
        pressed = inp(0x91) & 0x01;
        raw     = pressed ? inp(0x90) : 0;
		if (raw == 0x94) last_raw = 0;
        __asm__("ei");

        if (!pressed) { last_raw = 0; return 0; }
        
        if (raw == last_raw) return 0;       /* key held, already reported */
        last_raw = raw;
		
        /* Arrow keys */
        if (raw == 0xE2) return 'w';
        if (raw == 0xE3) return 's';
        if (raw == 0xE1) return 'a';
        if (raw == 0xE0) return 'd';
        /* Joystick */
        if (raw == 0xA8) return 'w';
        if (raw == 0xA2) return 's';
        if (raw == 0xA1) return 'a';
        if (raw == 0xA4) return 'd';
        if (raw == 0xB0) return 0x0D;
		
		if (raw == 'q' | raw =='Q') return 'q';
		if (raw == 0x0D) return 0xD;
        return 0;
    } else {
        /* ---- BIOS path ---- */
        uint8_t c = (uint8_t)bios(3, 0, 0);

        /* Cloud CP/M arrow codes */
        if (c == 0x05) return 'w';
        if (c == 0x18) return 's';
        if (c == 0x13) return 'a';
        if (c == 0x04) return 'd';
        /* Iskur arrow codes */
        if (c == 0x0B) return 'w';
        if (c == 0x0A) return 's';
        if (c == 0x08) return 'a';
        if (c == 0x0C) return 'd';

        if (c == 0) return 0;
        if (g_isVT100 && c == 0x1B) {
            uint8_t next = (uint8_t)bios(3, 0, 0);
            if (next == 0x5B) {
                uint8_t code = (uint8_t)bios(3, 0, 0);
                /* VT100 / ROMWBW arrows */
                if (code == 0x41) return 'w';
                if (code == 0x42) return 's';
                if (code == 0x44) return 'a';
                if (code == 0x43) return 'd';
            }
            return 0;
        }
        return c;
    }
}


/* -----------------------------------------------------------------------
   TNFS protocol exchange
   ----------------------------------------------------------------------- */
uint8_t tn_xchg(uint8_t cmd, uint16_t payLen) {
    g_txBuf[0] = (uint8_t)(g_sid & 0xff);
    g_txBuf[1] = (uint8_t)(g_sid >> 8);
    g_txBuf[2] = g_seq;
    g_txBuf[3] = cmd;
    uint16_t totalTX = (uint16_t)(4 + payLen);
    if (rn_TCPHandleWrite(g_tcp, 0, totalTX, g_txBuf) != (int32_t)totalTX) return 0;
    while (rn_TCPHandleSize(g_tcp) < 5) {}
    int32_t got = rn_TCPHandleRead(g_tcp, g_buf, 0, 1024);
    if (got < 5 || g_buf[2] != g_seq) { g_seq++; return 0; }
    g_sid = (uint16_t)g_buf[0] | ((uint16_t)g_buf[1] << 8);
    g_seq++;
    return 1;
}

/* -----------------------------------------------------------------------
   Execution helpers – two methods depending on platform.

   Standard CP/M: Write A:$$$.SUB so CCP runs the file on exit.
   Cloud CP/M:    Use BDOS 59 (P_LOAD) to load and jump directly.

   BDOS function numbers
   ----------------------------------------------------------------------- */
#define F_DELETE    19
#define F_MAKE      22
#define F_WRITE     21
#define F_CLOSE     16
#define F_OPEN      15
#define SET_DMA     26
#define P_LOAD      59   /* Cloud CP/M: load .COM at 0x0100 */
#define F_WRITRAND  34
#define F_SIZE      35

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include <stdio.h>
#include <string.h>

static void write_submit(void) {
    FILE *fp;
    uint8_t sub_buf[128];
    const char *cmd = "A:TNFSTEMP";
    uint8_t cmd_len = 10; // The exact length of "A:TNFSTEMP"

    // Try to open for appending in binary mode
    // If it fails (file doesn't exist), open for writing
    fp = fopen("A:$$$.SUB", "ab");
    if (!fp) {
        fp = fopen("A:$$$.SUB", "wb");
    }

    if (fp) {
        // 1. Zero out the 128-byte record
        memset(sub_buf, 0, 128);

        // 2. First byte is the length of the command
        sub_buf[0] = cmd_len;

        // 3. Copy the fixed command directly (no uppercase conversion needed)
        memcpy(&sub_buf[1], cmd, cmd_len);

        // 4. Write the full 128-byte sector
        fwrite(sub_buf, 1, 128, fp);
        fclose(fp);
    }
}

/* --- Cloud CP/M: BDOS 59 (P_LOAD) then jump to 0x0100 -------------- *
 * FCB layout (36 bytes):
 *   [0]     drive (0 = default)
 *   [1..8]  name  (8 chars, space-padded)
 *   [9..11] ext   (3 chars, space-padded)
 *   [12..31] EX,S1,S2,RC,disk map (20 bytes)
 *   [32]    CR    (current record)
 *   [33..34] load address (little-endian word) – 0x0100 for .COM
 *
 * Returns 1 on success (caller should jump to 0x0100),
 *         0 on file-not-found or load error (prints reason).
 * -------------------------------------------------------------------- */
static uint8_t pload_exec(const char *name8, const char *ext3) {
    /* Raw FCB – use a byte array to avoid struct layout uncertainty */
    static uint8_t fcb[36];
    uint8_t result;

    memset(fcb, 0, sizeof(fcb));
    /* Drive 0 = default already set by memset */
    memcpy(&fcb[1],  name8, 8);   /* 8-char space-padded stem   */
    memcpy(&fcb[9],  ext3,  3);   /* 3-char space-padded ext    */
    /* fcb[12..32] = 0 (EX/S1/S2/RC/map/CR) */
    fcb[33] = 0x00;  /* load address low  byte = 0x0100 */
    fcb[34] = 0x01;  /* load address high byte          */

    /* Step 1: open file (required by P_LOAD) */
    result = (uint8_t)bdos(F_OPEN, (unsigned int)fcb);
    if (result == 0xFF) {
        printf("\nFILE NOT ON CLOUD");
        return 0;
    }

    /* Step 2: P_LOAD */
    result = (uint8_t)bdos(P_LOAD, (unsigned int)fcb);
    if (result != 0) {
        printf("\nP_LOAD ERROR %02X", result);
        return 0;
    }

    /* Step 3: disable interrupts and jump to loaded program */
	__asm__("di");

    ((void (*)(void))0x0100)();

    return 1; /* unreachable, but silences warnings */
}

/* --- Convert a destName like "FOO.COM" into 8+3 space-padded fields -- */
static void split_83(const char *destName, char *name8, char *ext3) {
    const char *dot = strchr(destName, '.');
    uint8_t i;

    memset(name8, ' ', 8);
    memset(ext3,  ' ', 3);

    if (dot) {
        uint8_t stemLen = (uint8_t)(dot - destName);
        if (stemLen > 8) stemLen = 8;
        for (i = 0; i < stemLen; i++) name8[i] = destName[i];
        dot++;
        for (i = 0; i < 3 && dot[i]; i++) ext3[i] = dot[i];
    } else {
        uint8_t stemLen = (uint8_t)strlen(destName);
        if (stemLen > 8) stemLen = 8;
        for (i = 0; i < stemLen; i++) name8[i] = destName[i];
    }
}

/* --- Unified exec: choose method based on platform.
   Returns 1 only for standard CP/M ($$$.SUB written, caller should exit).
   Returns 0 for Cloud CP/M (either jumped and never returned, or failed).
   ----------------------------------------------------------------------- */
static uint8_t exec_file(const char *destName) {
    if (isCloudCPM()) {
        char name8[8], ext3[3];
        split_83(destName, name8, ext3);
        pload_exec(name8, ext3);
        /* Reaches here only on failure – error already printed by pload_exec */
        return 0;
    } else {
        /* Standard CP/M: strip extension for $$$.SUB command line */
        char cmd[9];
        const char *dot = strchr(destName, '.');
        uint8_t len = dot ? (uint8_t)(dot - destName) : (uint8_t)strlen(destName);
        if (len > 8) len = 8;
        memcpy(cmd, destName, len);
        cmd[len] = '\0';
        write_submit();
        return 1; /* Caller should exit so CCP picks up $$$.SUB */
    }
}

/* -----------------------------------------------------------------------
   Download a file by pointer
   Returns 1 if exec was requested and successful, 0 otherwise
   ----------------------------------------------------------------------- */
uint8_t do_download(FileEntry *fe, uint8_t exec) {
    FILE *fp = NULL;
    uint32_t totalBytes = 0;
    uint16_t readLen;
    uint8_t fh, origUser, origDrive, destUser = 0xff, destDrive = 0xff, success = 0;
    char fullPath[128], destName[36], inputBuf[34], *p, *dot; 
    
    if (exec) {
        // Silent mode: hardcode destination and bypass prompts
        strcpy(destName, "TNFSTEMP.COM");
    } else {
        // Standard mode: format default name and prompt user
        strncpy(destName, fe->name, 35); destName[35] = '\0';
        dot = strrchr(destName, '.');
        if (dot) {
            char ext[4];
            strncpy(ext, dot + 1, 3); ext[3] = '\0';
            *dot = '\0';
            if (strlen(destName) > 8) destName[8] = '\0';
            strcat(destName, "."); strcat(destName, ext);
        } else {
            if (strlen(destName) > 8) destName[8] = '\0';
        }
        { uint8_t i; for (i = 0; destName[i]; i++) destName[i] = my_toupper(destName[i]); }

        move_cursor(22, 0); printf("%-39s", "");
        move_cursor(23, 0); printf("%-39s", "");
        move_cursor(22, 0); printf("Save [%s]: ", destName);
        get_input(inputBuf, 31);
        if (inputBuf[0]) { strncpy(destName, inputBuf, 35); destName[35] = '\0'; }

        { uint8_t i; for (i = 0; destName[i]; i++) destName[i] = my_toupper(destName[i]); }
        p = strchr(destName, ':');
        if (p) {
            if (destName[0] >= 'A' && destName[0] <= 'P') {
                destDrive = (uint8_t)(destName[0] - 'A');
                if (my_isdigit((uint8_t)destName[1])) destUser = my_atoi8(&destName[1]);
            }
            memmove(destName, p + 1, strlen(p + 1) + 1);
        }
    }

    origUser  = (uint8_t)bdos(32, 0xFF);
    origDrive = (uint8_t)bdos(25, 0);
    if (destUser  != 0xff) bdos(32, destUser);
    if (destDrive != 0xff) {
        bdos(14, destDrive);
        char driveDest[35];
        driveDest[0] = (char)('A' + destDrive); driveDest[1] = ':';
        strncpy(&driveDest[2], destName, 32); driveDest[34] = '\0';
        strncpy(destName, driveDest, 35); destName[35] = '\0';
    }

    move_cursor(22, 0); printf("%-39s", "");
    move_cursor(23, 0); printf("%-39s", "");
    move_cursor(22, 0); printf("DL: %-32s", destName);

    strcpy(fullPath, g_currentPath);
    { uint8_t l = (uint8_t)strlen(fullPath); if (fullPath[l-1] != '/') { fullPath[l] = '/'; fullPath[l+1] = '\0'; } }
    strcat(fullPath, fe->name);

    g_txBuf[4] = 0x01; g_txBuf[5] = 0x00;
    strcpy((char *)&g_txBuf[6], fullPath);
    if (!tn_xchg(CMD_OPENFILE, (uint16_t)strlen(fullPath) + 3) || g_buf[4] != 0) goto cleanup;

    fh = g_buf[5]; fp = fopen(destName, "wb");
    if (!fp) goto cleanup;

    { uint8_t blk = 0;
      while (1) {
          g_txBuf[4] = fh; g_txBuf[5] = 0x00; g_txBuf[6] = 0x02;
          if (!tn_xchg(CMD_READFILE, 3)) break;
          if (g_buf[4] != 0) break;
          readLen = (uint16_t)g_buf[5] | ((uint16_t)g_buf[6] << 8);
          if (readLen == 0) break;
          fwrite(&g_buf[7], 1, readLen, fp);
          totalBytes += (uint32_t)readLen;
          if ((++blk & 7) == 0) { move_cursor(23, 0); printf("%-15lu", totalBytes); }
      }
    }
    fclose(fp);
    success = 1;
    g_txBuf[4] = fh; tn_xchg(CMD_CLOSEFILE, 1);
    move_cursor(23, 20); printf("DONE.");
    
    if (exec) {
		move_cursor(23, 20); printf("RUNNING");
        success = exec_file(destName); /* 1=standard CPM (exit to CCP), 0=cloud (jumped or failed) */
    }
    
cleanup:
    bdos(14, origDrive); bdos(32, origUser); 
    if (!exec || !success) getch(); /* Only skip pause if successfully chaining */
    return (exec && success);
}

/* -----------------------------------------------------------------------
   Directory paging helpers
   ----------------------------------------------------------------------- */

static uint8_t entry_group(const FileEntry *e) {
    if (e->name[0] == '.' && e->name[1] == '.' && e->name[2] == '\0') return 0;
    return e->isDir ? 1 : 2;
}

static void dir_close(void) {
    if (g_dirHandle != 0xff) {
        g_txBuf[4] = g_dirHandle;
        tn_xchg(CMD_CLOSEDIR, 1);
        g_dirHandle = 0xff;
    }
}

static uint8_t dir_open(void) {
    uint16_t pathLen = (uint16_t)strlen(g_currentPath);
    dir_close();

    if (g_useX != 0) {
        g_txBuf[4] = DIROPT_DEFAULT;        /* Default: let server hide ".." */
        g_txBuf[5] = DIRSORT_DEFAULT;       /* dirs first, name A-Z */
        g_txBuf[6] = 0x00; g_txBuf[7] = 0x00; 
        g_txBuf[8] = 0x00;                  
        strcpy((char *)&g_txBuf[9], g_currentPath);
        if (tn_xchg(CMD_OPENDIRX, (uint16_t)(5 + pathLen + 1))) {
            if (g_buf[4] == 0x00) {
                g_dirHandle = g_buf[5];
                g_useX = 1;
                return 1;
            }
            if (g_buf[4] != TNFS_ENOSYS) return 0; 
        }
        g_useX = 0;
    }

    strcpy((char *)&g_txBuf[4], g_currentPath);
    if (!tn_xchg(CMD_OPENDIR, (uint16_t)(pathLen + 1)) || g_buf[4] != 0)
        return 0;
    g_dirHandle = g_buf[5];
    return 1;
}

/* Updated dir_read_page: handles skip accurately by verifying each dropped item.
   Always injects ".." on page 0 and skips duplicates from the server. */
static void dir_read_page(uint16_t skip) {
    uint8_t i, j;
    g_fileCount = 0;

    /* --- Skip entries for page-back --- */
    if (g_useX) {
        while (skip > 0) {
            g_txBuf[4] = g_dirHandle; g_txBuf[5] = 1;
            if (!tn_xchg(CMD_READDIRX, 2) || g_buf[4] != 0) return;
            char *en = (char *)&g_buf[22];
            /* ignore server's "." or ".." entirely */
            if ((en[0] == '.' && en[1] == '\0') || (en[0] == '.' && en[1] == '.' && en[2] == '\0')) {
                if (g_buf[6] & DIRSTATUS_EOF) return;
                continue;
            }
            skip--;
            if (g_buf[6] & DIRSTATUS_EOF) return;
        }
    } else {
        while (skip > 0) {
            g_txBuf[4] = g_dirHandle;
            if (!tn_xchg(CMD_READDIR, 1) || g_buf[4] != 0) return;
            char *en = (char *)&g_buf[5];
            if ((en[0] == '.' && en[1] == '\0') || (en[0] == '.' && en[1] == '.' && en[2] == '\0')) continue;
            skip--;
        }
    }

    /* --- Inject ".." on the first page always --- */
    if (g_dirPage == 0) {
        FileEntry *fe = &g_fileStore[0];
        strcpy(fe->name, "..");
        fe->isDir = 1;
        g_fileList[0] = fe;
        g_fileCount = 1;
    }

    /* --- Read up to MAX_FILES entries --- */
    if (g_useX) {
        while (g_fileCount < MAX_FILES) {
            uint8_t dirstatus, eflags;
            char *en;
            g_txBuf[4] = g_dirHandle; g_txBuf[5] = 1;
            if (!tn_xchg(CMD_READDIRX, 2) || g_buf[4] != 0) break;
            
            dirstatus = g_buf[6];
            eflags    = g_buf[9];
            en        = (char *)&g_buf[22];

            /* Strip server-provided "." or ".." to prevent duplicates */
            if ((en[0] == '.' && en[1] == '\0') || (en[0] == '.' && en[1] == '.' && en[2] == '\0')) {
                if (dirstatus & DIRSTATUS_EOF) { g_hasMore = 0; return; }
                continue;
            }

            { FileEntry *fe = &g_fileStore[g_fileCount];
              strncpy(fe->name, en, 62); fe->name[62] = '\0';
              fe->isDir = (eflags & DIRENTRY_DIR) ? 1 : 0;
              g_fileList[g_fileCount] = fe;
              g_fileCount++;
            }

            if (dirstatus & DIRSTATUS_EOF) { g_hasMore = 0; return; }
        }
        
        g_hasMore = 0;
        if (g_fileCount == MAX_FILES) {
            g_txBuf[4] = g_dirHandle; g_txBuf[5] = 1;
            if (tn_xchg(CMD_READDIRX, 2) && g_buf[4] == 0) {
                if (!(g_buf[6] & DIRSTATUS_EOF) || g_buf[5] > 0) g_hasMore = 1;
            }
        }

    } else {
        char probePath[128];

        while (g_fileCount < MAX_FILES) {
            g_txBuf[4] = g_dirHandle;
            if (!tn_xchg(CMD_READDIR, 1) || g_buf[4] != 0) break;
            char *en = (char *)&g_buf[5];
            
            if ((en[0] == '.' && en[1] == '\0') || (en[0] == '.' && en[1] == '.' && en[2] == '\0')) continue;

            { FileEntry *fe = &g_fileStore[g_fileCount];
              strncpy(fe->name, en, 62); fe->name[62] = '\0';

              strcpy(probePath, g_currentPath);
              { uint8_t l = (uint8_t)strlen(probePath);
                if (probePath[l-1] != '/') { probePath[l] = '/'; probePath[l+1] = '\0'; } }
              strcat(probePath, en);
              strcpy((char *)&g_txBuf[4], probePath);
              if (tn_xchg(CMD_OPENDIR, (uint16_t)strlen(probePath) + 1) && g_buf[4] == 0x00) {
                  g_txBuf[4] = g_buf[5]; tn_xchg(CMD_CLOSEDIR, 1);
                  fe->isDir = 1;
              } else {
                  fe->isDir = 0;
              }
              g_fileList[g_fileCount] = fe;
              g_fileCount++;
            }
        }

        g_hasMore = 0;
        if (g_fileCount == MAX_FILES) {
            g_txBuf[4] = g_dirHandle;
            if (tn_xchg(CMD_READDIR, 1) && g_buf[4] == 0) g_hasMore = 1;
        }

        for (i = 1; i < g_fileCount; i++) {
            FileEntry *tmp = g_fileList[i];
            uint8_t gt = entry_group(tmp);
            j = i;
            while (j > 0) {
                FileEntry *a = g_fileList[j-1];
                uint8_t ga = entry_group(a);
                if (ga < gt) break;
                if (ga == gt && strcmp(a->name, tmp->name) <= 0) break;
                g_fileList[j] = g_fileList[j-1];
                j--;
            }
            g_fileList[j] = tmp;
        }
    }
}

void load_dir(void) {
    move_cursor(0, 0); printf("TNFS v0.4 | Loading... %-16s", "");
    g_dirPage = 0;
    if (!dir_open()) { g_fileCount = 0; g_hasMore = 0; return; }
    dir_read_page(0);
}

static void load_dir_next(void) {
    g_dirPage++;
    move_cursor(0, 0); printf("TNFS v0.4 | Loading... %-16s", "");
    dir_read_page(0);   
}

static void load_dir_prev(void) {
    if (g_dirPage == 0) return;
    g_dirPage--;
    move_cursor(0, 0); printf("TNFS v0.4 | Loading... %-16s", "");
    if (!dir_open()) { g_fileCount = 0; g_hasMore = 0; return; }
    /* Calculate precise amount of strictly real server files to skip */
    uint16_t skip = (g_dirPage == 0) ? 0 : ((uint16_t)g_dirPage * MAX_FILES) - 1;
    dir_read_page(skip);
}

/* -----------------------------------------------------------------------
   File browser display
   ----------------------------------------------------------------------- */
void full_redraw(uint8_t cursor) {
    uint8_t i;
    clear_screen(); move_cursor(0, 0);
    printf("TNFS v0.4 | %-26s", g_currentPath);
    for (i = 0; i < MAX_FILES; i++) {
        move_cursor((uint8_t)(i + 1), 0);
        if (i < g_fileCount) {
            FileEntry *fe = g_fileList[i];
            printf("%c %s %s", (i == cursor ? '>' : ' '),
                   fe->isDir ? "[D]" : "   ", fe->name);
        }
    }
    move_cursor(22, 0); printf("---------------------------------------");
    move_cursor(23, 0);
    printf("Joystick/Arrows/GO/Q ");
}

/* -----------------------------------------------------------------------
   Server list – flat buffer with pointer array
   ----------------------------------------------------------------------- */
void load_server_list(void) {
    FILE *fp;
    char *dst = g_serverBuf;
    char *end = g_serverBuf + SERVER_BUF_SIZE - 1;
    g_numServers = 0;

    fp = fopen("TNFS.TXT", "r");
    if (!fp) { goto use_default; }

    while (g_numServers < MAX_SERVERS && dst < end) {
        char line[64]; char *tmp;
        if (!fgets(line, 63, fp)) break;
        tmp = line; while (*tmp && !(*tmp == '\r' || *tmp == '\n')) tmp++;
        *tmp = '\0';
        while (tmp > line && my_isspace((uint8_t)*(tmp-1))) { tmp--; *tmp = '\0'; }
        uint8_t len = (uint8_t)strlen(line);
        if (len <= 3) continue;
        if (dst + len + 1 > end) break;
        g_servers[g_numServers++] = dst;
        memcpy(dst, line, len + 1);
        dst += len + 1;
    }
    fclose(fp);
    if (g_numServers > 0) return;

use_default:
    g_servers[0] = g_serverBuf;
    strcpy(g_serverBuf, "fujinet.online");
    g_numServers = 1;
}

void save_last_server(uint8_t idx) {
    FILE *fp = fopen("TNFS.INI", "w");
    if (!fp) return;
    if (idx >= 10) fputc('0' + idx / 10, fp);
    fputc('0' + idx % 10, fp);
    fputc('\n', fp);
    fclose(fp);
}

uint8_t load_last_server(void) {
    FILE *fp = fopen("TNFS.INI", "r");
    uint8_t v = 0; int c;
    if (!fp) return 0;
    while ((c = fgetc(fp)) != EOF && my_isdigit((uint8_t)c))
        v = (uint8_t)(v * 10 + (c - '0'));
    fclose(fp);
    if (v >= g_numServers) v = 0;
    return v;
}

/* -----------------------------------------------------------------------
   Server selection menu
   ----------------------------------------------------------------------- */
#define SERVER_PAGE 20

void draw_server_menu(uint8_t cursor, uint8_t offset) {
    uint8_t i; clear_screen();
    move_cursor(0, 0);
    printf("TNFS NABU v0.4 by GTAMP");
    for (i = 0; i < SERVER_PAGE; i++) {
        uint8_t idx = (uint8_t)(i + offset); move_cursor((uint8_t)(i + 2), 0);
        if (idx < g_numServers)
            printf("%c  %s", (idx == cursor ? '>' : ' '), g_servers[idx]);
    }
    move_cursor(22, 0); printf("-- %u/%-2u%-29s",
        (unsigned)(cursor + 1), (unsigned)g_numServers, "");
    move_cursor(23, 0); printf("Joystick/Arrows/GO/Q ");
}

uint8_t select_server(uint8_t initialCursor) {
    uint8_t cursor = initialCursor, offset = 0, lastOffset = 0, key, prevCursor;
    if (cursor >= SERVER_PAGE) offset = (uint8_t)(cursor - SERVER_PAGE + 1);
    draw_server_menu(cursor, offset);
    while (1) {
        key = get_key(); if (key == 0) continue;
        prevCursor = cursor;
        if ((key == 'w') && cursor > 0) {
            cursor--; if (cursor < offset) offset--;
        } else if ((key == 's') && cursor < (uint8_t)(g_numServers - 1)) {
            cursor++; if (cursor >= (uint8_t)(offset + SERVER_PAGE)) offset++;
        } else if (key == 'a') {
            if (cursor >= SERVER_PAGE) cursor -= SERVER_PAGE; else cursor = 0;
            if (cursor < offset) offset = cursor;
        } else if (key == 'd') {
            uint8_t nc = (uint8_t)(cursor + SERVER_PAGE);
            if (nc >= g_numServers) nc = (uint8_t)(g_numServers - 1);
            cursor = nc;
            if (cursor >= (uint8_t)(offset + SERVER_PAGE)) offset = (uint8_t)(cursor - SERVER_PAGE + 1);
        } else if (key == 0x0D) return cursor;
        else if (key == 'q') return 0xFF;

        if (cursor != prevCursor || offset != lastOffset) {
            if (offset != lastOffset) { draw_server_menu(cursor, offset); lastOffset = offset; }
            else {
                move_cursor((uint8_t)(prevCursor - offset + 2), 0); putchar(' ');
                move_cursor((uint8_t)(cursor   - offset + 2), 0); putchar('>');
                move_cursor(22, 0); printf("-- %u/%-2u%-29s",
                    (unsigned)(cursor + 1), (unsigned)g_numServers, "");
            }
        }
    }
}

/* -----------------------------------------------------------------------
   Main
   ----------------------------------------------------------------------- */
void main(int argc, char **argv) {
    uint8_t cursor, key, sIdx;

    /* /t flag: start in terminal/BIOS mode instead of raw hardware input */
    if (argc > 1 && argv[1][0] == '/' &&
        (argv[1][1] == 't' || argv[1][1] == 'T')) {
        g_rawKeys = 0;
    }

    detect_terminal();
    load_server_list();

    uint8_t lastServer = load_last_server();

    while (1) {
        sIdx = select_server(lastServer);
        if (sIdx == 0xFF) {
			// flush console/keyboard before exit
				while (bdos(11, 0) != 0) { // Function 11 is "Check Console Status"
					bdos(1, 0);            // Function 1 is "Read Console" (Consumes the char)
				}
				clear_screen();
				return; // Return to CPM
		}
        save_last_server(sIdx);
        lastServer = sIdx;

        cursor = 0; g_useX = 2; 
        strcpy(g_currentPath, "/"); g_seq = 0x00; g_sid = 0x0000;

        clear_screen(); move_cursor(0, 0);
        printf("Connecting to %s...", g_servers[sIdx]);

        g_tcp = rn_TCPOpen((uint8_t)strlen(g_servers[sIdx]), (uint8_t *)g_servers[sIdx], 16384, 0xff);
        if (g_tcp == 0xff) { printf("\nLink Down."); getch(); continue; }

        g_txBuf[4] = 0x02; g_txBuf[5] = 0x01;
        strcpy((char *)&g_txBuf[6], "/");
        g_txBuf[8] = 0x00; g_txBuf[9] = 0x00;
        if (tn_xchg(CMD_MOUNT, 6) && g_buf[4] == 0x00) {
            g_sid = (uint16_t)g_buf[0] | ((uint16_t)g_buf[1] << 8);
        } else {
            printf("\nMount Error %02X", g_buf[4]); getch();
            rn_TCPHandleClose(g_tcp); continue;
        }

        load_dir(); full_redraw(cursor);

        while (1) {
            key = get_key(); if (key == 0) continue;
            uint8_t prevCursor = cursor;
            uint8_t needRedraw = 0;

            if (key == 'w') {
                if (cursor > 0) { cursor--; }
                else if (g_dirPage > 0) { load_dir_prev(); cursor = g_fileCount - 1; needRedraw = 1; }
            } else if (key == 's') {
                if (cursor < (uint8_t)(g_fileCount - 1)) { cursor++; }
                else if (g_hasMore) { load_dir_next(); cursor = 0; needRedraw = 1; }
            } else if (key == 'a') {
                if (g_dirPage > 0) { load_dir_prev(); cursor = 0; needRedraw = 1; }
                else { cursor = 0; }
            } else if (key == 'd') {
                if (g_hasMore) { load_dir_next(); cursor = 0; needRedraw = 1; }
                else { cursor = (uint8_t)(g_fileCount - 1); }
            } else if (key == 'q') {
                dir_close();
                rn_TCPHandleClose(g_tcp); g_tcp = 0xff; break;
            } else if (key == 0x0D) {
                FileEntry *fe = g_fileList[cursor];
                
        if (fe->isDir) {
            // --- Directory Navigation ---
            if (fe->name[0] == '.' && fe->name[1] == '.' && fe->name[2] == '\0') {
                if (strcmp(g_currentPath, "/") == 0) {
                    /* We are already at the top, break out to the server list */
                    dir_close();
                    rn_TCPHandleClose(g_tcp); 
                    g_tcp = 0xff;
                    break; 
                }
                char *last = strrchr(g_currentPath, '/');
                if (last && last != g_currentPath) *last = '\0';
                else strcpy(g_currentPath, "/");
            } else {
                uint8_t l = (uint8_t)strlen(g_currentPath);
                if (g_currentPath[l-1] != '/') { g_currentPath[l] = '/'; g_currentPath[l+1] = '\0'; }
                strcat(g_currentPath, fe->name);
            }
            cursor = 0; load_dir(); needRedraw = 1;
        } else {
            // --- File Download & Execute ---
            uint8_t do_exec = 0;
            char *dot = strrchr(fe->name, '.');
            
            // Safe, case-insensitive check for .COM extension
            if (dot && 
               (dot[1] == 'C' || dot[1] == 'c') &&
               (dot[2] == 'O' || dot[2] == 'o') &&
               (dot[3] == 'M' || dot[3] == 'm') &&
               dot[4] == '\0') {
                do_exec = 1;
            }

            move_cursor(22, 0); printf("%-39s", "");
            
            // Call download with dynamically calculated execute flag
            do_download(fe, do_exec);
            
            if (do_exec) {
                dir_close();
				// flush console/keyboard before exit
				while (bdos(11, 0) != 0) { // Function 11 is "Check Console Status"
					bdos(1, 0);            // Function 1 is "Read Console" (Consumes the char)
				}
				rn_TCPHandleClose(g_tcp);
                return;
            } else {
                // If it wasn't a .COM file, we stay in the browser
                needRedraw = 1;
            }
        }
            }

            if (needRedraw) {
                full_redraw(cursor);
            } else if (g_fileCount > 0 && cursor != prevCursor) {
                move_cursor((uint8_t)(prevCursor + 1), 0); putchar(' ');
                move_cursor((uint8_t)(cursor     + 1), 0); putchar('>');
            }
        }
    }
}
