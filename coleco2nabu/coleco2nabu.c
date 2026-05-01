/*
 * coleco2nabu - Tries to convert ColecoVision roms to NABU PC
 * 
 * Copyright (c) 2026 GTAMP.  All rights reserved.
 * Coleco Loader code copyright (c) 2024 Brian Johnson.  All rights reserved.
 *
 * https://gtamp.com/nabu
 * https://github.com/gtampdotcom/NABU-PC-PROJECTS
 * https://github.com/brijohn/nabupc/blob/master/games/nabu_coleco/
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

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

// Modified to print individual patch addresses
int find_and_replace(uint8_t* data, size_t size, const uint8_t* search, const uint8_t* replace, size_t len, const char* desc) {
    int count = 0;
    for (size_t i = 0; i <= size - len; i++) {
        if (memcmp(&data[i], search, len) == 0) {
            printf("\n    [0x%04X] %s", (uint32_t)i, desc);
            memcpy(&data[i], replace, len);
            count++;
        }
    }
    return count;
}

void install_vector(uint8_t* rom, size_t rom_len, uint16_t offset, const uint8_t* data, const char* desc) {
    if (rom_len > offset + 3) {
        printf("\n    [0x%04X] Vector Table: %s", offset, desc);
        memcpy(&rom[offset], data, 3);
    }
}

int apply_contextual_patches(uint8_t* rom, size_t rom_len) {
    int count = 0;
    for (size_t i = 5; i < rom_len - 2; i++) {
        if (rom[i] == 0xED && rom[i+1] == 0x41) {
            for (int j = 1; j <= 5; j++) {
                if (rom[i-j] == 0x3E && rom[i-j+1] == 0xFF) {
                    printf("\n    [0x%04X] Contextual Sound Fix (OUT (C),B sequence)", (uint32_t)(i-j));
                    rom[i-j] = 0x00; rom[i-j+1] = 0x00;
                    for (int k = i-j+2; k < i; k++) {
                        if (rom[k] == 0x4F) rom[k] = 0x00;
                    }
                    rom[i] = 0x78; rom[i+1] = 0xF7; 
                    count++;
                    break; 
                }
            }
        }
    }
    if (count > 0) {
        install_vector(rom, rom_len, vec_patches[0].vector_offset, vec_patches[0].vector_data, "Sound Context");
    }
    return count;
}

void apply_auto_patches(uint8_t* rom, size_t rom_len, int swap_joy) {
    printf("\n  Applying Standard Patches:");
    for (int i = 0; i < 8; i++) {
        find_and_replace(rom, rom_len, std_patches[i].search, std_patches[i].replace, std_patches[i].len, std_patches[i].desc);
    }

    printf("\n  Applying Contextual Patches:");
    apply_contextual_patches(rom, rom_len);

    printf("\n  Applying Vector Patches:");
    for (int i = 0; i < 6; i++) {
        int found = find_and_replace(rom, rom_len, vec_patches[i].search, vec_patches[i].replace, 2, vec_patches[i].desc);
        if (found > 0) {
            uint8_t v_data[3];
            memcpy(v_data, vec_patches[i].vector_data, 3);
            if (swap_joy) {
                if (v_data[1] == 0x06) v_data[1] = 0x09;
                else if (v_data[1] == 0x09) v_data[1] = 0x06;
            }
            install_vector(rom, rom_len, vec_patches[i].vector_offset, v_data, vec_patches[i].desc);
        }
    }
}

int apply_external_patches(uint8_t* rom, size_t rom_len, uint16_t target_crc) {
    FILE* f = fopen("patches.z80", "r");
    if (!f) return 0;
    char line[512], target_label[128] = {0}, crc_search[16];
    sprintf(crc_search, "0x%04x", target_crc);
    int found_game = 0;
    while (fgets(line, sizeof(line), f)) {
        char temp[512]; strncpy(temp, line, 511);
        for(int i=0; temp[i]; i++) temp[i] = (char)tolower((unsigned char)temp[i]);
        if (strstr(temp, crc_search)) {
            while (fgets(line, sizeof(line), f)) {
                if (strstr(line, ".word")) {
                    sscanf(strstr(line, ".word") + 5, "%127s", target_label);
                    found_game = 1; break;
                }
            }
            if (found_game) break;
        }
    }
    if (!found_game) { fclose(f); return 0; }
    printf("\n  [CRC MATCH]: %s", target_label);
    rewind(f);
    int in_label = 0;
    uint16_t current_f_off = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!in_label) {
            char *p = line; while(isspace(*p)) p++;
            if (strncmp(p, target_label, strlen(target_label)) == 0) in_label = 1;
            continue;
        }
        char *ptr = line; while(isspace(*ptr)) ptr++;
        if (strncmp(ptr, ".word", 5) == 0) {
            uint32_t val = parse_hex(ptr + 5);
            if (val == 0) break;
            if (val >= 0x8000) {
                current_f_off = (uint16_t)(val - 0x8000);
                printf("\n    [0x%04X] Target Pointer Set", current_f_off);
            }
            else if (current_f_off + 1 < rom_len) {
                rom[current_f_off++] = val & 0xFF;
                rom[current_f_off++] = (val >> 8) & 0xFF;
            }
        } 
        else if (strncmp(ptr, ".byte", 5) == 0) {
            char *p_byte = ptr + 5;
            int count = (int)strtol(p_byte, &p_byte, 0);
            for (int b = 0; b < count && current_f_off < rom_len; b++) {
                while (*p_byte && !isxdigit(*p_byte)) p_byte++;
                if (*p_byte) {
                    rom[current_f_off++] = (uint8_t)parse_hex(p_byte);
                    while (isxdigit(*p_byte) || *p_byte == 'x' || *p_byte == 'X') p_byte++;
                }
            }
        }
    }
    fclose(f); return 1;
}

void process_file(const char* filename, uint8_t* stub_data, size_t stub_len, int skip_patches, int swap_joy) {
    FILE* f = fopen(filename, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); size_t rom_len = ftell(f); fseek(f, 0, SEEK_SET);
    if (rom_len > MAX_ROM_SIZE) { printf("Processing: %-32s OVER 32KB, SKIPPED\n", filename); fclose(f); return; }
    uint8_t* rom = (uint8_t*)malloc(rom_len);
    fread(rom, 1, rom_len, f); fclose(f);
    uint16_t crc = calculate_crc16(rom, rom_len);
    printf("Processing: %-32s (CRC: 0x%04X)", filename, crc);
    if (!skip_patches) {
        if (!apply_external_patches(rom, rom_len, crc)) apply_auto_patches(rom, rom_len, swap_joy);
    }
    char base[1024]; strncpy(base, filename, 1023);
    char* dot = strrchr(base, '.'); if (dot) *dot = '\0';
    size_t stub_off = stub_len - MAX_ROM_SIZE;
    size_t pay_len = stub_off + rom_len;
    uint8_t* nabu_out = (uint8_t*)malloc(pay_len);
    if (nabu_out) {
        memcpy(nabu_out, stub_data, stub_off);
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
    free(rom);
}

int main(int argc, char* argv[]) {
    printf("coleco2nabu v0.2 by GTAMP (c) 2026\n");
    printf("based on Coleco Loader by Brian Johnson\n");
    int skip = 0, swap = 0, stub_v = 1, f_count = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-nopatch")) skip = 1;
        else if (!strcmp(argv[i], "-swapjoy")) swap = 1;
        else if (!strcmp(argv[i], "-2")) stub_v = 2;
        else f_count++;
    }
    if (f_count == 0) { printf("\nUsage: coleco2nabu <roms> [-2] [-nopatch] [-swapjoy]\n"); return 1; }

    char stub_name[32];
    sprintf(stub_name, "coleco2nabu.00%d", stub_v);
    FILE* ts = fopen(stub_name, "rb");
    if (!ts) { 
        ts = fopen("coleco2nabu.bin", "rb");
        if (!ts) { printf("Error: Stub file missing.\n"); return 1; }
    }
    printf("Using Stub: %s\n", stub_name);

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
                process_file(full, sd, sl, skip, swap); 
            } } while (_findnext(h, &cf) == 0);
            _findclose(h);
        }
    }
    free(sd); return 0;
}