/*
 * RetroNet Get (RGET) v0.4 by GTAMP (c) 2026
 */

#define BIN_TYPE BIN_CPM
#define DISABLE_VDP 
#define DISABLE_KEYBOARD_INT

#include <stdio.h>
#include <stdlib.h>
#include <conio.h>
#include "../../NABULIB/RetroNET-FileStore.h"
#include "../../NABULIB/NABU-LIB.h"
#include <string.h>
#include <ctype.h>

#define CPM_CMD_LEN  (*(uint8_t *)0x0080)
#define CPM_CMD_TEXT ((char *)0x0081)
#define BUFFERSIZE 128

#ifndef OPEN_FILE_FLAG_READONLY
#define OPEN_FILE_FLAG_READONLY 0x01 
#endif

uint8_t _buffer[BUFFERSIZE];
uint8_t bdosBuffer[130]; 
char rawUrl[128];
char rawFile[128];
char pathPart[128];
char wildPart[32];
char fullRemote[160];

/* --- Helpers --- */

void safeExit() {
  __asm
    ei
    jp 0x0000
  __endasm;
}

void showHelp() {
    printf("\nUsage: RGET [URL] [filename]\n\n");
    printf("Examples:\n");
    printf("RGET gtamp.com/nabu/vgmp.com v.com\n");
    printf("RGET gtamp.com/nabu/oilswell.com\n");
    printf("RGET ia: (list directory)\n");
    printf("RGET ia:file.com\n");
    printf("RGET ia:vgm/*.vgm (batch download)\n");
    printf("RGET /U (Update RGET from gtamp.com)\n");
    printf("RGET (no args for mixed case URL)\n");
    safeExit();
}

uint8_t fileExists(const char *name) {
    FILE *f = fopen(name, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

void abortProg(uint8_t h, FILE *f, const char *msg) {
  if (h != 0xff) rn_fileHandleClose(h);
  if (f != NULL) fclose(f);
  printf("\n%s\n", msg);
  safeExit();
}

void getLine(char* dest, uint8_t maxLen) {
  bdosBuffer[0] = maxLen; bdosBuffer[1] = 0;
  __asm
    ld de, #_bdosBuffer
    ld c, #10
    call 5
  __endasm;
  uint8_t len = bdosBuffer[1]; 
  memcpy(dest, &bdosBuffer[2], len); 
  dest[len] = '\0';
  printf("\n"); 
}

/* --- Directory & IA Logic --- */

void doDirectory(char* path) {
    uint16_t count, i;
    FileDetailsStruct entry;
    char cleanPath[128];
    
    if (strncmp(path, "ia:", 3) == 0) strcpy(cleanPath, path + 3);
    else strcpy(cleanPath, path);

    i = (uint16_t)strlen(cleanPath);
    if (i > 1 && (cleanPath[i-1] == '\\' || cleanPath[i-1] == '/')) cleanPath[i-1] = '\0';
    if (cleanPath[0] == '\0') strcpy(cleanPath, "\\");

    count = rn_fileList((uint8_t)strlen(cleanPath), cleanPath, 3, "*.*", 
                        FILE_LIST_FLAG_INCLUDE_FILES | FILE_LIST_FLAG_INCLUDE_DIRECTORIES);
    
    if (count == 0) {
        printf("Error: Not Found\n");
        return;
    }

    printf("Listing: %s\n", cleanPath);
    for (i = 0; i < count; i++) {
        rn_fileListItem(i, &entry);
        if (entry.IsFile) printf("%-12s  %ldb\n", entry.Filename, entry.FileSize);
        else printf("[%s]\n", entry.Filename);
    }
    printf("%u item(s).\n\n", count);
}

/* --- Download Core --- */

uint8_t downloadFile(const char* path, const char* localName, uint8_t skipCheck) {
    uint8_t handle = 0xff;
    uint16_t readBytes = 0;
    uint32_t currentFileBytes = 0;
    uint8_t pct = 0, barChars = 0, i;
    uint8_t updateCounter = 0;
    FileDetailsStruct details;
    FILE *fp = NULL;
    char dirPart[128], filePart[32];
    char *ptr;

    memset(&details, 0, sizeof(FileDetailsStruct));

    if (!skipCheck && path[0] != 'h' && !strchr(path, '*') && !strchr(path, '?')) {
        ptr = strrchr(path, '\\');
        if (!ptr) ptr = strrchr(path, '/');
        if (!ptr) { strcpy(dirPart, "\\"); strcpy(filePart, path); }
        else {
            int pos = (int)(ptr - path);
            strncpy(dirPart, path, (size_t)pos); dirPart[pos] = '\0';
            strcpy(filePart, ptr + 1);
        }
        if (dirPart[0] == '\0') strcpy(dirPart, "\\");

        i = rn_fileList((uint8_t)strlen(dirPart), dirPart, (uint8_t)strlen(filePart), filePart, FILE_LIST_FLAG_INCLUDE_FILES);
        if (i == 0) { printf("Error: File Not Found\n"); return 0; }
        
        rn_fileListItem(0, &details);
        if (details.IsFile && details.FileSize == 0) { 
            printf("Error: File is 0 bytes\n"); 
            return 0; 
        }
    }

    handle = rn_fileOpen((uint8_t)strlen(path), (char*)path, OPEN_FILE_FLAG_READONLY, 0xff);
    if (handle == 0xff) { printf("Error: 404\n"); return 0; }

    rn_fileHandleDetails(handle, &details);
    
    if (details.FileSize <= 0) {
        rn_fileHandleClose(handle);
        printf("Error: Invalid File Size\n");
        return 0;
    }

    fp = fopen(localName, "wb");
    if (!fp) { rn_fileHandleClose(handle); printf("Error: Disk Error\n"); return 0; }

    printf("Saving: %-12s (%ldb)\n", localName, details.FileSize);

    while (currentFileBytes < (uint32_t)details.FileSize) {
        if (kbhit() && getch() == 0x03) abortProg(handle, fp, "ABORTED");
        readBytes = rn_fileHandleReadSeq(handle, _buffer, 0, BUFFERSIZE);
        if (readBytes == 0) break; 
        if (fwrite(_buffer, 1, readBytes, fp) != readBytes) {
           fclose(fp); remove(localName); rn_fileHandleClose(handle);
           printf("\nError: Disk Full!\n"); return 0;
        }
        currentFileBytes += (uint32_t)readBytes;
        updateCounter++;

        /* 8 packets * 128 bytes = 1024 bytes */
        if (updateCounter >= 8 || currentFileBytes >= (uint32_t)details.FileSize) {
            updateCounter = 0;
            pct = (uint8_t)((currentFileBytes * 100) / details.FileSize);
            barChars = pct / 10;
            printf("\r[");
            for(i = 0; i < 10; i++) {
                if (i < barChars) putchar('=');
                else if (i == barChars) putchar('>');
                else putchar(' ');
            }
            printf("] %3u%% %ldb  ", pct, currentFileBytes);
        }
    }
    fclose(fp);
    rn_fileHandleClose(handle);
    printf("\r[==========>] 100%% - OK          \n\n");
    return 1;
}

/* --- Main --- */

void main() {
    FileDetailsStruct entry;
    uint16_t i, k, count;
    char finalUrl[160], finalLocal[16], workBuf[160];
    uint8_t yn;

    printf("RetroNet Get v0.4 by GTAMP (c) 2026\n");
    printf("Type RGET /? for help\n\n");

    rawUrl[0] = '\0'; rawFile[0] = '\0';

    if (CPM_CMD_LEN > 0) {
        i = 0; while (i < CPM_CMD_LEN && (uint8_t)CPM_CMD_TEXT[i] <= 32) i++;
        k = 0; while (i < CPM_CMD_LEN && (uint8_t)CPM_CMD_TEXT[i] > 32) 
            rawUrl[k++] = (char)tolower((uint8_t)CPM_CMD_TEXT[i++]);
        rawUrl[k] = '\0';
        while (i < CPM_CMD_LEN && (uint8_t)CPM_CMD_TEXT[i] <= 32) i++;
        k = 0; while (i < CPM_CMD_LEN && (uint8_t)CPM_CMD_TEXT[i] > 32) 
            rawFile[k++] = (char)toupper((uint8_t)CPM_CMD_TEXT[i++]);
        rawFile[k] = '\0';
    } else {
        printf("URL: "); getLine(workBuf, 159);
        i = 0; while (workBuf[i] && (uint8_t)workBuf[i] <= 32) i++;
        k = 0; while (workBuf[i] && (uint8_t)workBuf[i] > 32) rawUrl[k++] = workBuf[i++];
        rawUrl[k] = '\0';
        while (workBuf[i] && (uint8_t)workBuf[i] <= 32) i++;
        k = 0; while (workBuf[i] && (uint8_t)workBuf[i] > 32) rawFile[k++] = (char)toupper((uint8_t)workBuf[i++]);
        rawFile[k] = '\0';
    }

    if (strcmp(rawUrl, "/?") == 0) showHelp();
    if (rawUrl[0] == '\0') safeExit();
    
    if (strcmp(rawUrl, "/u") == 0 || strcmp(rawUrl, "/U") == 0) {
        downloadFile("https://gtamp.com/nabu/rget.com", "RGET.COM", 1);
        safeExit();
    }

    if (strncmp(rawUrl, "ia:", 3) == 0 || strncmp(rawUrl, "IA:", 3) == 0) {
        if (strchr(rawUrl, '*') || strchr(rawUrl, '?')) {
            char *ptr = strrchr(rawUrl, '\\');
            if (!ptr) ptr = strrchr(rawUrl, '/');
            if (!ptr) { strcpy(pathPart, "\\"); strcpy(wildPart, rawUrl + 3); }
            else {
                int pos = (int)(ptr - rawUrl);
                strncpy(pathPart, rawUrl + 3, (size_t)(pos - 3)); pathPart[pos - 3] = '\0';
                strcpy(wildPart, ptr + 1);
            }
            if (pathPart[0] == '\0') strcpy(pathPart, "\\");
            count = rn_fileList((uint8_t)strlen(pathPart), pathPart, (uint8_t)strlen(wildPart), wildPart, FILE_LIST_FLAG_INCLUDE_FILES);
            if (count > 0) {
                printf("Batch download %u files (Y/N)? ", count);
                yn = (uint8_t)toupper(getch()); printf("%c\n\n", yn);
                if (yn != 'Y') safeExit();
                for (i = 0; i < count; i++) {
                    rn_fileListItem(i, &entry);
                    if (fileExists(entry.Filename)) {
                        printf("Skipping: %-12s (Exists)\n\n", entry.Filename);
                        continue;
                    }
                    strcpy(fullRemote, pathPart);
                    if (fullRemote[strlen(fullRemote)-1] != '\\') strcat(fullRemote, "\\");
                    strcat(fullRemote, entry.Filename);
                    downloadFile(fullRemote, entry.Filename, 1);
                }
                safeExit();
            } else {
                printf("Error: File Not Found\n");
                safeExit();
            }
        }

        i = (uint16_t)strlen(rawUrl);
        if (rawUrl[i-1] == '/' || rawUrl[i-1] == '\\' || !strchr(rawUrl + 3, '.')) {
             doDirectory(rawUrl); safeExit();
        }
    }

    if (strncmp(rawUrl, "ia:", 3) == 0 || strncmp(rawUrl, "IA:", 3) == 0) {
        strcpy(finalUrl, rawUrl + 3);
    } else if (strncasecmp(rawUrl, "http", 4) == 0) {
        strcpy(finalUrl, rawUrl);
    } else {
        strcpy(finalUrl, "https://");
        strcat(finalUrl, rawUrl);
    }

    if (rawFile[0] != '\0') strncpy(finalLocal, rawFile, 12);
    else {
        char *ptr = (strncasecmp(rawUrl, "ia:", 3) == 0) ? rawUrl + 3 : rawUrl;
        char *base = strrchr(ptr, '/');
        if (!base) base = strrchr(ptr, '\\');
        strncpy(finalLocal, (base) ? base + 1 : ptr, 12);
    }
    finalLocal[12] = '\0';
    for(i=0; i<strlen(finalLocal); i++) finalLocal[i] = (char)toupper((uint8_t)finalLocal[i]);

    if (fileExists(finalLocal)) {
        printf("%s exists. Overwrite (Y/N)? ", finalLocal);
        yn = (uint8_t)toupper(getch()); printf("%c\n", yn);
        if (yn != 'Y') safeExit();
    }

    downloadFile(finalUrl, finalLocal, 0);
    safeExit();
}
