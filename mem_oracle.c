/*
mem_oracle.c - RAM/memory-controller correctness oracle (Windows x64)

Usage examples:
  mem_oracle.exe --size-mb 4096 --passes 1 --mode both --threads 8 --seed 1234
  mem_oracle.exe --size-mb 1024 --mode multi --threads 16 --affinity on --page-random on
  mem_oracle.exe --help
*/

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

enum Pattern {
    PAT_ZERO = 0,
    PAT_ONES,
    PAT_AA,
    PAT_55,
    PAT_WALK1,
    PAT_WALK0,
    PAT_PRNG,
    PAT_COUNT
};

enum AccessMode {
    ACCESS_SEQ = 0,
    ACCESS_CL64,
    ACCESS_PAGE4096,
    ACCESS_PAGE_RANDOM,
    ACCESS_COUNT
};

enum RunMode {
    RUN_SINGLE = 0,
    RUN_MULTI,
    RUN_BOTH
};

typedef struct FailureInfo {
    LONG failed;
    const char* pattern_name;
    const char* access_name;
    const char* phase;
    uint64_t byte_offset;
    uint64_t expected;
    uint64_t actual;
    uint32_t thread_index;
} FailureInfo;

typedef struct RunConfig {
    uint64_t size_bytes;
    uint32_t threads;
    uint64_t seed;
    uint32_t passes;
    enum RunMode mode;
    int affinity;
    int page_random;
} RunConfig;

typedef struct Shared {
    uint64_t* words;
    uint64_t word_count;
    uint64_t page_count;
    uint64_t* page_perm;
    const RunConfig* cfg;
    FailureInfo* fail;
    enum Pattern pattern;
    enum AccessMode access;
    const char* pattern_name;
    const char* access_name;
    const char* phase;
    uint32_t round;
} Shared;

typedef struct Worker {
    Shared* sh;
    uint32_t tidx;
    uint64_t start_word;
    uint64_t end_word;
} Worker;

static uint64_t splitmix64_step(uint64_t x) {
    x += UINT64_C(0x9E3779B97F4A7C15);
    x = (x ^ (x >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94D049BB133111EB);
    x ^= (x >> 31);
    return x;
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
    if (strcmp(s, "on") == 0) {
        *out = 1;
        return 1;
    }
    if (strcmp(s, "off") == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

static int parse_mode(const char* s, enum RunMode* out) {
    if (strcmp(s, "single") == 0) {
        *out = RUN_SINGLE;
        return 1;
    }
    if (strcmp(s, "multi") == 0) {
        *out = RUN_MULTI;
        return 1;
    }
    if (strcmp(s, "both") == 0) {
        *out = RUN_BOTH;
        return 1;
    }
    return 0;
}

static const char* pattern_name(enum Pattern p) {
    static const char* names[PAT_COUNT] = {
        "zeros", "ones", "alt_aa", "alt_55", "walking1", "walking0", "prng"
    };
    return names[p];
}

static const char* access_name(enum AccessMode a) {
    static const char* names[ACCESS_COUNT] = {
        "sequential", "stride64", "stride4096", "page_random"
    };
    return names[a];
}

static uint64_t pattern_value(enum Pattern p, uint64_t abs_word, uint64_t seed, uint32_t round) {
    uint64_t bit = UINT64_C(1) << (abs_word & 63u);
    switch (p) {
        case PAT_ZERO: return UINT64_C(0);
        case PAT_ONES: return UINT64_C(0xFFFFFFFFFFFFFFFF);
        case PAT_AA:   return UINT64_C(0xAAAAAAAAAAAAAAAA);
        case PAT_55:   return UINT64_C(0x5555555555555555);
        case PAT_WALK1:return bit;
        case PAT_WALK0:return ~bit;
        case PAT_PRNG: {
            uint64_t x = seed ^ UINT64_C(0xD1B54A32D192ED03) * (abs_word + 1u);
            x ^= UINT64_C(0x9E3779B97F4A7C15) * (uint64_t)(round + 1u);
            return splitmix64_step(x);
        }
        default: return 0;
    }
}

static void set_failure(Shared* sh, uint32_t tidx, uint64_t abs_word, uint64_t expected, uint64_t actual) {
    if (InterlockedCompareExchange(&sh->fail->failed, 1, 0) == 0) {
        sh->fail->pattern_name = sh->pattern_name;
        sh->fail->access_name = sh->access_name;
        sh->fail->phase = sh->phase;
        sh->fail->byte_offset = abs_word * 8u;
        sh->fail->expected = expected;
        sh->fail->actual = actual;
        sh->fail->thread_index = tidx;
    }
}

static int do_fill_or_verify(Worker* w, int verify) {
    Shared* sh = w->sh;
    uint64_t* words = sh->words;
    uint64_t start = w->start_word;
    uint64_t end = w->end_word;

    if (sh->access == ACCESS_PAGE_RANDOM) {
        for (uint64_t pi = 0; pi < sh->page_count; ++pi) {
            uint64_t page = sh->page_perm[pi];
            uint64_t page_word_start = page * 512u;
            uint64_t page_word_end = page_word_start + 512u;
            if (page_word_start >= end || page_word_end <= start) continue;
            uint64_t s = (page_word_start < start) ? start : page_word_start;
            uint64_t e = (page_word_end > end) ? end : page_word_end;
            for (uint64_t i = s; i < e; ++i) {
                uint64_t exp = pattern_value(sh->pattern, i, sh->cfg->seed, sh->round);
                if (!verify) {
                    words[i] = exp;
                } else {
                    uint64_t act = words[i];
                    if (act != exp) {
                        set_failure(sh, w->tidx, i, exp, act);
                        return 0;
                    }
                }
            }
        }
        return 1;
    }

    uint64_t stride = 1;
    if (sh->access == ACCESS_CL64) stride = 8;
    if (sh->access == ACCESS_PAGE4096) stride = 512;

    for (uint64_t off = 0; off < stride; ++off) {
        uint64_t i = start + off;
        if (i >= end) continue;
        for (; i < end; i += stride) {
            uint64_t exp = pattern_value(sh->pattern, i, sh->cfg->seed, sh->round);
            if (!verify) {
                words[i] = exp;
            } else {
                uint64_t act = words[i];
                if (act != exp) {
                    set_failure(sh, w->tidx, i, exp, act);
                    return 0;
                }
            }
        }
    }
    return 1;
}

static DWORD WINAPI worker_main(LPVOID p) {
    Worker* w = (Worker*)p;
    if (w->sh->cfg->affinity) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        DWORD n = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
        DWORD_PTR mask = ((DWORD_PTR)1u) << (w->tidx % n);
        SetThreadAffinityMask(GetCurrentThread(), mask);
    }
    if (!do_fill_or_verify(w, 0)) return 1;
    if (!do_fill_or_verify(w, 1)) return 2;
    return 0;
}

static int build_page_permutation(uint64_t* perm, uint64_t count, uint64_t seed) {
    for (uint64_t i = 0; i < count; ++i) perm[i] = i;
    if (count <= 1) return 1;
    uint64_t s = seed;
    for (uint64_t i = count - 1; i > 0; --i) {
        s = splitmix64_step(s ^ i);
        uint64_t j = s % (i + 1u);
        uint64_t tmp = perm[i]; perm[i] = perm[j]; perm[j] = tmp;
    }
    return 1;
}

static int run_case(Shared* sh, uint32_t threads) {
    sh->fail->failed = 0;

    if (threads == 1) {
        Worker w;
        w.sh = sh;
        w.tidx = 0;
        w.start_word = 0;
        w.end_word = sh->word_count;
        if (!do_fill_or_verify(&w, 0)) return 0;
        if (!do_fill_or_verify(&w, 1)) return 0;
        return 1;
    }

    Worker* workers = (Worker*)calloc(threads, sizeof(Worker));
    HANDLE* hs = (HANDLE*)calloc(threads, sizeof(HANDLE));
    if (!workers || !hs) {
        fprintf(stderr, "ERROR: worker allocation failed\n");
        free(workers); free(hs);
        return 0;
    }

    uint64_t base = sh->word_count / threads;
    uint64_t rem = sh->word_count % threads;
    uint64_t cur = 0;

    for (uint32_t t = 0; t < threads; ++t) {
        uint64_t len = base + (t < rem ? 1u : 0u);
        workers[t].sh = sh;
        workers[t].tidx = t;
        workers[t].start_word = cur;
        workers[t].end_word = cur + len;
        cur += len;
        hs[t] = CreateThread(NULL, 0, worker_main, &workers[t], 0, NULL);
        if (!hs[t]) {
            fprintf(stderr, "ERROR: CreateThread failed t=%u gle=%lu\n", t, GetLastError());
            for (uint32_t j = 0; j < t; ++j) {
                WaitForSingleObject(hs[j], INFINITE);
                CloseHandle(hs[j]);
            }
            free(workers); free(hs);
            return 0;
        }
    }

    WaitForMultipleObjects(threads, hs, TRUE, INFINITE);
    for (uint32_t t = 0; t < threads; ++t) CloseHandle(hs[t]);
    free(workers);
    free(hs);

    return sh->fail->failed == 0;
}

static void print_help(const char* exe) {
    printf("Usage: %s [options]\n", exe);
    printf("  --size-mb <n>       Buffer size in MiB (default: 4096)\n");
    printf("  --threads <n>       Thread count for multi mode (default: 4)\n");
    printf("  --seed <u64>        Deterministic seed (default: 0xC001D00D12345678)\n");
    printf("  --passes <n>        Number of full suites (default: 1)\n");
    printf("  --mode single|multi|both (default: both)\n");
    printf("  --affinity on|off   Pin threads (default: off)\n");
    printf("  --page-random on|off Include randomized page order access mode (default: on)\n");
    printf("  --help\n");
}

int main(int argc, char** argv) {
    RunConfig cfg;
    cfg.size_bytes = UINT64_C(4096) * 1024u * 1024u;
    cfg.threads = 4;
    cfg.seed = UINT64_C(0xC001D00D12345678);
    cfg.passes = 1;
    cfg.mode = RUN_BOTH;
    cfg.affinity = 0;
    cfg.page_random = 1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--size-mb") == 0 && i + 1 < argc) {
            uint64_t mb = 0;
            if (!parse_u64(argv[++i], &mb) || mb == 0) {
                fprintf(stderr, "ERROR: invalid --size-mb\n");
                return 2;
            }
            cfg.size_bytes = mb * 1024u * 1024u;
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], &cfg.threads) || cfg.threads == 0) {
                fprintf(stderr, "ERROR: invalid --threads\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &cfg.seed)) {
                fprintf(stderr, "ERROR: invalid --seed\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--passes") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], &cfg.passes) || cfg.passes == 0) {
                fprintf(stderr, "ERROR: invalid --passes\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            if (!parse_mode(argv[++i], &cfg.mode)) {
                fprintf(stderr, "ERROR: --mode must be single|multi|both\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--affinity") == 0 && i + 1 < argc) {
            if (!parse_on_off(argv[++i], &cfg.affinity)) {
                fprintf(stderr, "ERROR: --affinity must be on|off\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--page-random") == 0 && i + 1 < argc) {
            if (!parse_on_off(argv[++i], &cfg.page_random)) {
                fprintf(stderr, "ERROR: --page-random must be on|off\n");
                return 2;
            }
        } else {
            fprintf(stderr, "ERROR: unknown or incomplete argument: %s\n", argv[i]);
            return 2;
        }
    }

    if ((cfg.size_bytes % 8u) != 0u) cfg.size_bytes -= (cfg.size_bytes % 8u);
    if (cfg.size_bytes < 8u) {
        fprintf(stderr, "ERROR: size too small\n");
        return 2;
    }

    uint8_t* raw = (uint8_t*)VirtualAlloc(NULL, (SIZE_T)cfg.size_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!raw) {
        fprintf(stderr, "ERROR: VirtualAlloc failed size=%" PRIu64 " gle=%lu\n", cfg.size_bytes, GetLastError());
        return 3;
    }

    Shared sh;
    FailureInfo fail;
    memset(&sh, 0, sizeof(sh));
    memset(&fail, 0, sizeof(fail));

    sh.words = (uint64_t*)raw;
    sh.word_count = cfg.size_bytes / 8u;
    sh.page_count = cfg.size_bytes / 4096u;
    sh.cfg = &cfg;
    sh.fail = &fail;

    if (cfg.page_random && sh.page_count > 0) {
        sh.page_perm = (uint64_t*)malloc((size_t)(sh.page_count * sizeof(uint64_t)));
        if (!sh.page_perm) {
            fprintf(stderr, "ERROR: page permutation allocation failed\n");
            VirtualFree(raw, 0, MEM_RELEASE);
            return 3;
        }
        build_page_permutation(sh.page_perm, sh.page_count, cfg.seed ^ UINT64_C(0x55AA11EE));
    }

    uint32_t thread_modes[2] = {1, cfg.threads};
    uint32_t mode_count = (cfg.mode == RUN_BOTH) ? 2u : 1u;
    if (cfg.mode == RUN_SINGLE) thread_modes[0] = 1;
    if (cfg.mode == RUN_MULTI) thread_modes[0] = cfg.threads;

    for (uint32_t round = 0; round < cfg.passes; ++round) {
        for (enum Pattern p = PAT_ZERO; p < PAT_COUNT; ++p) {
            for (enum AccessMode a = ACCESS_SEQ; a < ACCESS_COUNT; ++a) {
                if (a == ACCESS_PAGE_RANDOM && !cfg.page_random) continue;
                for (uint32_t mi = 0; mi < mode_count; ++mi) {
                    uint32_t tc = thread_modes[mi];
                    sh.pattern = p;
                    sh.access = a;
                    sh.pattern_name = pattern_name(p);
                    sh.access_name = access_name(a);
                    sh.round = round;
                    printf("PASS_START round=%u pattern=%s access=%s threading=%s threads=%u\n",
                           round, sh.pattern_name, sh.access_name, tc == 1 ? "single" : "multi", tc);
                    sh.phase = "fill_verify";
                    if (!run_case(&sh, tc)) {
                        fprintf(stderr,
                                "FAIL pattern=%s access=%s phase=%s round=%u thread=%u offset=%" PRIu64
                                " expected=0x%016" PRIx64 " actual=0x%016" PRIx64 "\n",
                                fail.pattern_name, fail.access_name, fail.phase, round, fail.thread_index,
                                fail.byte_offset, fail.expected, fail.actual);
                        if (sh.page_perm) free(sh.page_perm);
                        VirtualFree(raw, 0, MEM_RELEASE);
                        printf("RESULT: FAIL\n");
                        return 1;
                    }
                    printf("PASS_END round=%u pattern=%s access=%s threading=%s status=PASS\n",
                           round, sh.pattern_name, sh.access_name, tc == 1 ? "single" : "multi");
                }
            }
        }
    }

    if (sh.page_perm) free(sh.page_perm);
    VirtualFree(raw, 0, MEM_RELEASE);
    printf("RESULT: PASS\n");
    return 0;
}
