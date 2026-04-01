/* NABU CP/M TNFS client v0.1 by GTAMP (c) 2026
   Trivial/Tiny/Taco Network File System, primarily used by Fujinet.
   https://github.com/gtampdotcom/NABU-PC-PROJECTS
   https://gtamp.com/nabu
   MIT license
*/

#define BIN_TYPE          BIN_CPM
#define DISABLE_VDP
#define DISABLE_KEYBOARD_INT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <conio.h>
#include <cpm.h>
#include "../../NABULIB/NABU-LIB.h"
#include "../../NABULIB/RetroNET-FileStore.h"

#define PAGE_SIZE   21 
#define MAX_SERVERS 50

#define CMD_MOUNT      0x00
#define CMD_OPENDIR    0x10
#define CMD_READDIR    0x11
#define CMD_CLOSEDIR   0x12
#define CMD_OPENFILE   0x20
#define CMD_READFILE   0x21
#define CMD_CLOSEFILE  0x23 

static uint8_t g_isVT100 = 0;

static uint8_t  g_buf[1024];   
static uint8_t  g_txBuf[1024]; 
static uint8_t  g_tcp = 0xff;
static uint16_t g_sid = 0x0000;
static uint8_t  g_seq = 0x00;
static char     g_currentPath[128] = "/";

typedef struct { char name[64]; uint8_t isDir; } FileEntry;
static FileEntry g_fileList[150];
static uint8_t   g_fileCount = 0;

char g_servers[MAX_SERVERS][64];
uint8_t g_numServers = 0;

void detect_terminal() {
    volatile uint8_t *ioBytePtr = (volatile uint8_t *)0x0003;
    uint8_t val = *ioBytePtr;
    g_isVT100 = (val == 0x01 || val == 0x93 || val == 0x94 || val == 0x9B || val == 0xFF) ? 1 : 0;
}

void clear_screen() {
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
        } else if (pos < maxLen - 1 && isprint(c)) {
            buffer[pos++] = (char)c; putchar(c);
        }
    }
    buffer[pos] = '\0'; printf("\n");
}

uint8_t get_key() {
    if (inp(0x91) & 0x01) {
        uint8_t raw = inp(0x90);
        if (raw == 0xE1) return 0x81; 
        if (raw == 0xE0) return 0x82; 
    }
    uint8_t c = (uint8_t)bios(3, 0, 0);
    if (c == 0) return 0;
    if (g_isVT100 && c == 0x1B) {
        uint8_t next = (uint8_t)bios(3, 0, 0);
        if (next == 0x5B) {
            uint8_t code = (uint8_t)bios(3, 0, 0);
            if (code == 0x41) return 0x81;
            if (code == 0x42) return 0x82;
        }
        return 0;
    }
    return c;
}

uint8_t tn_xchg(uint8_t cmd, uint16_t payLen) {
    g_txBuf[0] = (uint8_t)(g_sid & 0xff);
    g_txBuf[1] = (uint8_t)(g_sid >> 8);
    g_txBuf[2] = g_seq;
    g_txBuf[3] = cmd;
    uint16_t totalTX = 4 + payLen;
    if (rn_TCPHandleWrite(g_tcp, 0, totalTX, g_txBuf) != (int32_t)totalTX) return 0;
    while (rn_TCPHandleSize(g_tcp) < 5) {}
    int32_t got = rn_TCPHandleRead(g_tcp, g_buf, 0, 1024);
    if (got < 5 || g_buf[2] != g_seq) { g_seq++; return 0; }
    g_sid = (uint16_t)g_buf[0] | ((uint16_t)g_buf[1] << 8);
    g_seq++;
    return 1;
}

void do_download(uint8_t index) {
    FILE *fp; uint32_t totalBytes = 0; uint16_t readLen;
    uint8_t fh, origUser, origDrive, destUser = 0xff, destDrive = 0xff;
    char fullPath[128], destName[32], inputBuf[32], temp[64], *p, *dot;

    strcpy(temp, g_fileList[index].name);
    dot = strrchr(temp, '.');
    if (dot) {
        *dot = '\0'; strncpy(destName, temp, 8); destName[8] = '\0';
        strcat(destName, "."); strncat(destName, dot + 1, 3);
    } else {
        strncpy(destName, temp, 8); destName[8] = '\0';
    }
    for(int i = 0; destName[i]; i++) destName[i] = toupper(destName[i]);

    move_cursor(22, 0); printf("%-39s", "");
    move_cursor(23, 0); printf("%-39s", "");
    move_cursor(22, 0); printf("Save [%s]: ", destName);
    get_input(inputBuf, 31);
    if (strlen(inputBuf) > 0) strcpy(destName, inputBuf);

    for(int i = 0; destName[i]; i++) destName[i] = toupper(destName[i]);
    p = strchr(destName, ':');
    if (p) {
        if (destName[0] >= 'A' && destName[0] <= 'P') {
            destDrive = destName[0] - 'A';
            if (isdigit(destName[1])) destUser = (uint8_t)atoi(&destName[1]);
        }
        memmove(destName, p + 1, strlen(p)); 
    }

    origUser = bdos(32, 0xFF); origDrive = bdos(25, 0);
    if (destDrive != 0xff) bdos(14, destDrive);
    if (destUser != 0xff) bdos(32, destUser);

    move_cursor(22, 0); printf("%-39s", "");
    move_cursor(22, 0); printf("DL: %-32s", destName);
    
    strcpy(fullPath, g_currentPath);
    if (fullPath[strlen(fullPath)-1] != '/') strcat(fullPath, "/");
    strcat(fullPath, g_fileList[index].name);

    g_txBuf[4] = 0x01; g_txBuf[5] = 0x00;
    strcpy((char*)&g_txBuf[6], fullPath);
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
    } }
    fclose(fp); g_txBuf[4] = fh; tn_xchg(CMD_CLOSEFILE, 1);
    move_cursor(23, 20); printf("DONE.");
cleanup:
    bdos(14, origDrive); bdos(32, origUser); getch();
}

void load_dir() {
    uint8_t dh; g_fileCount = 0;
    move_cursor(0, 0); printf("TNFS v0.1 | Scanning...%-16s", "");
    strcpy((char*)&g_txBuf[4], g_currentPath);
    if (!tn_xchg(CMD_OPENDIR, (uint16_t)strlen(g_currentPath) + 1) || g_buf[4] != 0) return;
    dh = g_buf[5];
    while (g_fileCount < 150) {
        g_txBuf[4] = dh;
        if (!tn_xchg(CMD_READDIR, 1) || g_buf[4] != 0) break;
        char *entryName = (char*)&g_buf[5];
        if (strcmp(entryName, ".") == 0) continue;
        strncpy(g_fileList[g_fileCount].name, entryName, 63);
        g_fileList[g_fileCount].isDir = (strchr(entryName, '.') == NULL || strcmp(entryName, "..") == 0);
        g_fileCount++;
    }
    g_txBuf[4] = dh; tn_xchg(CMD_CLOSEDIR, 1);

    { uint8_t i, j; FileEntry tmp;
    for (i = 1; i < g_fileCount; i++) {
        tmp = g_fileList[i]; j = i;
        while (j > 0) {
            FileEntry *a = &g_fileList[j-1]; FileEntry *b = &tmp;
            uint8_t ga = (strcmp(a->name,"..") == 0) ? 0 : (a->isDir ? 1 : 2);
            uint8_t gb = (strcmp(b->name,"..") == 0) ? 0 : (b->isDir ? 1 : 2);
            if (ga < gb) break;
            if (ga == gb && strcmp(a->name, b->name) <= 0) break;
            g_fileList[j] = *a; j--;
        }
        g_fileList[j] = tmp;
    } }
}

void full_redraw(uint8_t cursor, uint8_t offset) {
    clear_screen(); move_cursor(0, 0);
    printf("TNFS v0.1 | Path: %-20s", g_currentPath);
    for (uint8_t i = 0; i < PAGE_SIZE; i++) {
        uint8_t idx = i + offset; move_cursor(i + 1, 0);
        if (idx < g_fileCount) {
            /* file/dir rows: not capped - content can exceed 39 cols */
            printf("%c %s %s", (idx == cursor ? '>' : ' '),
                   g_fileList[idx].isDir ? "[D]" : "   ", g_fileList[idx].name);
        }
    }
    move_cursor(22, 0); printf("---------------------------------------");
    move_cursor(23, 0); printf("W/S:Move  A/D:Page  ENTER/Q            ");
}

void load_server_list() {
    FILE *fp = fopen("TNFS.TXT", "r");
    char line[64], *tmp; g_numServers = 0;
    if (!fp) { strcpy(g_servers[0], "fujinet.online"); g_numServers = 1; return; }
    while (g_numServers < MAX_SERVERS && fgets(line, 63, fp)) {
        tmp = strpbrk(line, "\r\n"); if (tmp) *tmp = '\0';
        int len = strlen(line);
        while(len > 0 && isspace(line[len-1])) { line[--len] = '\0'; }
        if (len > 3) { strcpy(g_servers[g_numServers++], line); }
    }
    fclose(fp);
    if (g_numServers == 0) { strcpy(g_servers[0], "fujinet.online"); g_numServers = 1; }
}

#define SERVER_PAGE 20

void draw_server_menu(uint8_t cursor, uint8_t offset) {
    uint8_t i; clear_screen();
    move_cursor(0, 0);
    printf("TNFS NABU v0.1 by GTAMP | %s", g_isVT100 ? "VT100" : "ADM-3A");
    for (i = 0; i < SERVER_PAGE; i++) {
        uint8_t idx = i + offset; move_cursor(i + 2, 0);
        if (idx < g_numServers)
            /* server name rows: content, not capped */
            printf("%c  %s", (idx == cursor ? '>' : ' '), g_servers[idx]);
    }
    move_cursor(22, 0); printf("-- %u/%-2u%-29s",
        (unsigned)(cursor + 1), (unsigned)g_numServers, "");
    move_cursor(23, 0); printf("W/S:Move A/D:Page ENTER:Connect Q:Quit");
}

uint8_t select_server() {
    uint8_t cursor = 0, offset = 0, lastOffset = 0, key, prevCursor;
    draw_server_menu(cursor, offset);
    while (1) {
        key = get_key(); if (key == 0) continue;
        prevCursor = cursor;
        /* W / up arrow : up 1 */
        if ((key == 'w' || key == 'W' || key == 0x05 || key == 0x81) && cursor > 0) {
            cursor--; if (cursor < offset) offset--;
        /* S / down arrow : down 1 */
        } else if ((key == 's' || key == 'S' || key == 0x18 || key == 0x82) && cursor < g_numServers - 1) {
            cursor++; if (cursor >= offset + SERVER_PAGE) offset++;
        /* A : page up */
        } else if (key == 'a' || key == 'A') {
            if (cursor >= SERVER_PAGE) cursor -= SERVER_PAGE; else cursor = 0;
            if (cursor < offset) offset = cursor;
        /* D : page down */
        } else if (key == 'd' || key == 'D') {
            uint8_t nc = cursor + SERVER_PAGE;
            if (nc >= g_numServers) nc = g_numServers - 1;
            cursor = nc;
            if (cursor >= offset + SERVER_PAGE) offset = cursor - SERVER_PAGE + 1;
        } else if (key == 0x0D) return cursor;
        else if (key == 'q' || key == 'Q') return 0xFF;

        if (cursor != prevCursor || offset != lastOffset) {
            if (offset != lastOffset) { draw_server_menu(cursor, offset); lastOffset = offset; }
            else {
                move_cursor((prevCursor - offset) + 2, 0); putchar(' ');
                move_cursor((cursor - offset) + 2, 0); putchar('>');
                move_cursor(22, 0); printf("-- %u/%-2u%-29s", (unsigned)(cursor + 1), (unsigned)g_numServers, "");
            }
        }
    }
}

void main(void) {
    uint8_t cursor, viewOffset, lastOffset, key, sIdx;
    detect_terminal(); 
    load_server_list();

    while (1) {
        sIdx = select_server();
        if (sIdx == 0xFF) { clear_screen(); return; }

        cursor = 0; viewOffset = 0; lastOffset = 0;
        strcpy(g_currentPath, "/"); g_seq = 0x00; g_sid = 0x0000;

        clear_screen(); move_cursor(0, 0);
        printf("Connecting to %s...", g_servers[sIdx]);

        g_tcp = rn_TCPOpen((uint8_t)strlen(g_servers[sIdx]), (uint8_t *)g_servers[sIdx], 16384, 0xff);
        if (g_tcp == 0xff) { printf("\nLink Down."); getch(); continue; }

        g_txBuf[4] = 0x02; g_txBuf[5] = 0x01; 
        strcpy((char*)&g_txBuf[6], "/");
        g_txBuf[8] = 0x00; g_txBuf[9] = 0x00;
        if (tn_xchg(CMD_MOUNT, 6) && g_buf[4] == 0x00) {
            g_sid = (uint16_t)g_buf[0] | ((uint16_t)g_buf[1] << 8);
        } else {
            printf("\nMount Error %02X", g_buf[4]); getch(); 
            rn_TCPHandleClose(g_tcp); continue;
        }

        load_dir(); full_redraw(cursor, viewOffset);

        while (1) {
            key = get_key(); if (key == 0) continue;
            uint8_t prevCursor = cursor;

            /* W / up arrow : up 1 */
            if (key == 'w' || key == 'W' || key == 0x05 || key == 0x81) {
                if (cursor > 0) { cursor--; if (cursor < viewOffset) viewOffset--; }
            /* S / down arrow : down 1 */
            } else if (key == 's' || key == 'S' || key == 0x18 || key == 0x82) {
                if (cursor < g_fileCount - 1) { cursor++; if (cursor >= viewOffset + PAGE_SIZE) viewOffset++; }
            /* A : page up */
            } else if (key == 'a' || key == 'A') {
                if (cursor >= PAGE_SIZE) cursor -= PAGE_SIZE; else cursor = 0;
                if (cursor < viewOffset) viewOffset = cursor;
            /* D : page down */
            } else if (key == 'd' || key == 'D') {
                uint8_t nc = cursor + PAGE_SIZE;
                if (nc >= g_fileCount) nc = g_fileCount - 1;
                cursor = nc;
                if (cursor >= viewOffset + PAGE_SIZE) viewOffset = cursor - PAGE_SIZE + 1;
            /* Q : disconnect, back to server list */
            } else if (key == 'q' || key == 'Q') {
                rn_TCPHandleClose(g_tcp); g_tcp = 0xff; break;
            /* ENTER: enter dir, or Y/N download prompt for file */
            } else if (key == 0x0D) {
                if (g_fileList[cursor].isDir) {
                    if (strcmp(g_fileList[cursor].name, "..") == 0) {
                        char *last = strrchr(g_currentPath, '/');
                        if (last && last != g_currentPath) *last = '\0';
                        else strcpy(g_currentPath, "/");
                    } else {
                        if (g_currentPath[strlen(g_currentPath)-1] != '/') strcat(g_currentPath, "/");
                        strcat(g_currentPath, g_fileList[cursor].name);
                    }
                    cursor = 0; viewOffset = 0; load_dir(); full_redraw(cursor, viewOffset);
                } else {
                    uint8_t yn;
                    move_cursor(22, 0); printf("%-39s", "");
                    move_cursor(22, 0); printf("Download %s? (Y/N) ", g_fileList[cursor].name);
                    yn = get_key();
                    if (yn == 'y' || yn == 'Y') do_download(cursor);
                    full_redraw(cursor, viewOffset);
                }
            }

            if (g_fileCount > 0 && (viewOffset != lastOffset || cursor != prevCursor)) {
                if (viewOffset != lastOffset) { full_redraw(cursor, viewOffset); lastOffset = viewOffset; }
                else {
                    move_cursor((prevCursor - viewOffset) + 1, 0); putchar(' ');
                    move_cursor((cursor - viewOffset) + 1, 0); putchar('>');
                }
            }
        }
    }
}
