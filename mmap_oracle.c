/*
mmap_oracle.c - file-backed mapped-memory oracle (Windows x64)

Usage examples:
  mmap_oracle.exe --size-mb 2048 --seed 77 --cycles 3 --random-order on --flush on
  mmap_oracle.exe --path C:\\temp\\mmap_test.bin --size-mb 512 --cycles 5 --flush off
  mmap_oracle.exe --help
*/

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static const uint64_t PAGE_SIZE_U64 = 4096u;

typedef struct Config {
    char path[MAX_PATH];
    int path_given;
    uint64_t size_bytes;
    uint64_t seed;
    uint32_t cycles;
    int random_order;
    int flush;
} Config;

static uint64_t splitmix64_step(uint64_t x) {
    x += UINT64_C(0x9E3779B97F4A7C15);
    x = (x ^ (x >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94D049BB133111EB);
    x ^= (x >> 31);
    return x;
}

static uint64_t rotl64(uint64_t x, uint32_t r) {
    r &= 63u;
    if (r == 0u) return x;
    return (x << r) | (x >> (64u - r));
}

static int parse_u64(const char* s, uint64_t* out) {
    char* end = NULL;
    unsigned long long v = strtoull(s, &end, 0);
    if (s[0] == '\0' || (end && *end != '\0')) return 0;
    *out = (uint64_t)v;
    return 1;
}

static int parse_u32(const char* s, uint32_t* out) {
    uint64_t v = 0;
    if (!parse_u64(s, &v) || v > UINT32_MAX) return 0;
    *out = (uint32_t)v;
    return 1;
}

static int parse_on_off(const char* s, int* out) {
    if (strcmp(s, "on") == 0) { *out = 1; return 1; }
    if (strcmp(s, "off") == 0) { *out = 0; return 1; }
    return 0;
}

static void print_help(const char* exe) {
    printf("Usage: %s [options]\n", exe);
    printf("  --path <file>       Path for test file (default: auto temp file)\n");
    printf("  --size-mb <n>       File size in MiB (default: 512)\n");
    printf("  --seed <u64>        Deterministic seed (default: 0x0123456789ABCDEF)\n");
    printf("  --cycles <n>        Number of remap cycles (default: 1)\n");
    printf("  --random-order on|off Page traversal order (default: off)\n");
    printf("  --flush on|off      FlushViewOfFile/FlushFileBuffers after write (default: on)\n");
    printf("  --help\n");
}

static uint64_t expected_word(uint64_t seed, uint64_t page_idx, uint64_t word_in_page) {
    uint64_t x = seed;
    x ^= UINT64_C(0xD6E8FEB86659FD93) * (page_idx + 1u);
    x ^= UINT64_C(0xA5A3564E27F88651) * (word_in_page + 1u);
    return splitmix64_step(x);
}

static void build_page_order(uint64_t* perm, uint64_t page_count, uint64_t seed, int random_order) {
    for (uint64_t i = 0; i < page_count; ++i) perm[i] = i;
    if (!random_order || page_count <= 1) return;
    uint64_t s = seed;
    for (uint64_t i = page_count - 1; i > 0; --i) {
        s = splitmix64_step(s ^ i);
        uint64_t j = s % (i + 1u);
        uint64_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
}

static int create_temp_path(char out[MAX_PATH]) {
    char temp_dir[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, temp_dir);
    if (n == 0 || n > MAX_PATH - 1) return 0;
    UINT u = GetTempFileNameA(temp_dir, "mmo", 0, out);
    return u != 0;
}

int main(int argc, char** argv) {
    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.size_bytes = UINT64_C(512) * 1024u * 1024u;
    cfg.seed = UINT64_C(0x0123456789ABCDEF);
    cfg.cycles = 1;
    cfg.random_order = 0;
    cfg.flush = 1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
            strncpy(cfg.path, argv[++i], MAX_PATH - 1);
            cfg.path[MAX_PATH - 1] = '\0';
            cfg.path_given = 1;
        } else if (strcmp(argv[i], "--size-mb") == 0 && i + 1 < argc) {
            uint64_t mb = 0;
            if (!parse_u64(argv[++i], &mb) || mb == 0) {
                fprintf(stderr, "ERROR: invalid --size-mb\n");
                return 2;
            }
            cfg.size_bytes = mb * 1024u * 1024u;
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &cfg.seed)) {
                fprintf(stderr, "ERROR: invalid --seed\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--cycles") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], &cfg.cycles) || cfg.cycles == 0) {
                fprintf(stderr, "ERROR: invalid --cycles\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--random-order") == 0 && i + 1 < argc) {
            if (!parse_on_off(argv[++i], &cfg.random_order)) {
                fprintf(stderr, "ERROR: --random-order must be on|off\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--flush") == 0 && i + 1 < argc) {
            if (!parse_on_off(argv[++i], &cfg.flush)) {
                fprintf(stderr, "ERROR: --flush must be on|off\n");
                return 2;
            }
        } else {
            fprintf(stderr, "ERROR: unknown or incomplete argument: %s\n", argv[i]);
            return 2;
        }
    }

    if ((cfg.size_bytes % PAGE_SIZE_U64) != 0u) {
        cfg.size_bytes += PAGE_SIZE_U64 - (cfg.size_bytes % PAGE_SIZE_U64);
    }

    if (!cfg.path_given) {
        if (!create_temp_path(cfg.path)) {
            fprintf(stderr, "ERROR: failed to create temp path gle=%lu\n", GetLastError());
            return 3;
        }
    }

    uint64_t page_count = cfg.size_bytes / PAGE_SIZE_U64;
    uint64_t* page_order = (uint64_t*)malloc((size_t)(page_count * sizeof(uint64_t)));
    if (!page_order) {
        fprintf(stderr, "ERROR: allocation failed for page order\n");
        return 3;
    }

    int exit_code = 0;
    HANDLE hfile = INVALID_HANDLE_VALUE;

    hfile = CreateFileA(cfg.path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (hfile == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "ERROR: CreateFile failed path=%s gle=%lu\n", cfg.path, GetLastError());
        free(page_order);
        return 3;
    }

    LARGE_INTEGER fsize;
    fsize.QuadPart = (LONGLONG)cfg.size_bytes;
    if (!SetFilePointerEx(hfile, fsize, NULL, FILE_BEGIN) || !SetEndOfFile(hfile)) {
        fprintf(stderr, "ERROR: failed to resize file to %" PRIu64 " bytes gle=%lu\n", cfg.size_bytes, GetLastError());
        CloseHandle(hfile);
        if (!cfg.path_given) DeleteFileA(cfg.path);
        free(page_order);
        return 3;
    }

    for (uint32_t cycle = 0; cycle < cfg.cycles; ++cycle) {
        build_page_order(page_order, page_count, cfg.seed ^ (uint64_t)cycle, cfg.random_order);

        HANDLE hmap = CreateFileMappingA(hfile, NULL, PAGE_READWRITE,
                                         (DWORD)(cfg.size_bytes >> 32), (DWORD)(cfg.size_bytes & 0xFFFFFFFFu), NULL);
        if (!hmap) {
            fprintf(stderr, "ERROR: CreateFileMapping(write) failed cycle=%u gle=%lu\n", cycle, GetLastError());
            exit_code = 4;
            break;
        }

        uint8_t* view = (uint8_t*)MapViewOfFile(hmap, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, 0);
        if (!view) {
            fprintf(stderr, "ERROR: MapViewOfFile(write) failed cycle=%u gle=%lu\n", cycle, GetLastError());
            CloseHandle(hmap);
            exit_code = 4;
            break;
        }

        uint64_t write_digest = splitmix64_step(cfg.seed ^ cycle ^ UINT64_C(0xABCDEF01));
        for (uint64_t oi = 0; oi < page_count; ++oi) {
            uint64_t p = page_order[oi];
            uint64_t page_base = p * PAGE_SIZE_U64;
            uint64_t* w = (uint64_t*)(view + page_base);
            for (uint64_t wi = 0; wi < (PAGE_SIZE_U64 / 8u); ++wi) {
                uint64_t v = expected_word(cfg.seed ^ cycle, p, wi);
                w[wi] = v;
                write_digest ^= v + UINT64_C(0x9E3779B97F4A7C15) * (p + 1u);
                write_digest = rotl64(write_digest, (uint32_t)((wi + p) & 63u));
                write_digest *= UINT64_C(0xBF58476D1CE4E5B9);
            }
        }

        if (cfg.flush) {
            if (!FlushViewOfFile(view, 0) || !FlushFileBuffers(hfile)) {
                fprintf(stderr, "ERROR: flush failed cycle=%u gle=%lu\n", cycle, GetLastError());
                UnmapViewOfFile(view);
                CloseHandle(hmap);
                exit_code = 5;
                break;
            }
        }

        UnmapViewOfFile(view);
        CloseHandle(hmap);

        hmap = CreateFileMappingA(hfile, NULL, PAGE_READONLY,
                                  (DWORD)(cfg.size_bytes >> 32), (DWORD)(cfg.size_bytes & 0xFFFFFFFFu), NULL);
        if (!hmap) {
            fprintf(stderr, "ERROR: CreateFileMapping(read) failed cycle=%u gle=%lu\n", cycle, GetLastError());
            exit_code = 6;
            break;
        }

        view = (uint8_t*)MapViewOfFile(hmap, FILE_MAP_READ, 0, 0, 0);
        if (!view) {
            fprintf(stderr, "ERROR: MapViewOfFile(read) failed cycle=%u gle=%lu\n", cycle, GetLastError());
            CloseHandle(hmap);
            exit_code = 6;
            break;
        }

        uint64_t read_digest = splitmix64_step(cfg.seed ^ cycle ^ UINT64_C(0xABCDEF01));
        int mismatch = 0;
        for (uint64_t oi = 0; oi < page_count && !mismatch; ++oi) {
            uint64_t p = page_order[oi];
            uint64_t page_base = p * PAGE_SIZE_U64;
            uint64_t* w = (uint64_t*)(view + page_base);
            for (uint64_t wi = 0; wi < (PAGE_SIZE_U64 / 8u); ++wi) {
                uint64_t exp = expected_word(cfg.seed ^ cycle, p, wi);
                uint64_t act = w[wi];
                read_digest ^= act + UINT64_C(0x9E3779B97F4A7C15) * (p + 1u);
                read_digest = rotl64(read_digest, (uint32_t)((wi + p) & 63u));
                read_digest *= UINT64_C(0xBF58476D1CE4E5B9);
                if (act != exp) {
                    uint8_t* eb = (uint8_t*)&exp;
                    uint8_t* ab = (uint8_t*)&act;
                    uint32_t boff = 0;
                    while (boff < 8u && eb[boff] == ab[boff]) ++boff;
                    fprintf(stderr,
                            "FAIL cycle=%u page=%" PRIu64 " byte_in_page=%" PRIu64 " expected=0x%02x actual=0x%02x"
                            " expected_word=0x%016" PRIx64 " actual_word=0x%016" PRIx64 "\n",
                            cycle, p, wi * 8u + boff,
                            (unsigned)(boff < 8u ? eb[boff] : 0u),
                            (unsigned)(boff < 8u ? ab[boff] : 0u),
                            exp, act);
                    mismatch = 1;
                    break;
                }
            }
        }

        if (!mismatch && read_digest != write_digest) {
            fprintf(stderr,
                    "FAIL cycle=%u digest_mismatch write=0x%016" PRIx64 " read=0x%016" PRIx64 "\n",
                    cycle, write_digest, read_digest);
            mismatch = 1;
        }

        printf("cycle=%u write_digest=0x%016" PRIx64 " read_digest=0x%016" PRIx64 " status=%s\n",
               cycle, write_digest, read_digest, mismatch ? "FAIL" : "PASS");

        UnmapViewOfFile(view);
        CloseHandle(hmap);

        if (mismatch) {
            exit_code = 1;
            break;
        }
    }

    CloseHandle(hfile);
    if (!cfg.path_given) DeleteFileA(cfg.path);
    free(page_order);

    if (exit_code == 0) {
        printf("RESULT: PASS\n");
        return 0;
    }
    printf("RESULT: FAIL\n");
    return exit_code;
}
