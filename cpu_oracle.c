/*
cpu_oracle.c - deterministic multithreaded CPU correctness oracle (Windows x64)

Usage examples:
  cpu_oracle.exe --threads 8 --iters 20000000 --seed 12345
  cpu_oracle.exe --threads 16 --iters 5000000 --affinity on --yield-every 10000
  cpu_oracle.exe --help
*/

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

typedef struct WorkerArgs {
    uint32_t index;
    uint64_t iters;
    uint64_t global_seed;
    uint64_t yield_every;
    int use_affinity;
    uint64_t final_state;
    uint64_t digest;
    int thread_ok;
} WorkerArgs;

static uint64_t rotl64(uint64_t x, uint32_t r) {
    r &= 63u;
    if (r == 0u) return x;
    return (x << r) | (x >> (64u - r));
}

static uint64_t splitmix64_step(uint64_t x) {
    x += UINT64_C(0x9E3779B97F4A7C15);
    x = (x ^ (x >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94D049BB133111EB);
    x ^= (x >> 31);
    return x;
}

static uint64_t derive_seed(uint64_t global_seed, uint32_t worker_index, uint64_t iters) {
    uint64_t x = global_seed;
    x ^= UINT64_C(0xD6E8FEB86659FD93) * (uint64_t)(worker_index + 1u);
    x ^= UINT64_C(0xA5A3564E27F88651) * (iters + UINT64_C(0x9E3779B97F4A7C15));
    return splitmix64_step(x);
}

static void run_oracle_core(uint64_t start_seed, uint64_t iters, uint64_t yield_every,
                            uint64_t* out_final, uint64_t* out_digest) {
    uint64_t s = start_seed;
    uint64_t d = splitmix64_step(start_seed ^ UINT64_C(0x243F6A8885A308D3));

    for (uint64_t i = 0; i < iters; ++i) {
        uint64_t m = splitmix64_step(s + i * UINT64_C(0x9E3779B97F4A7C15));
        s ^= rotl64(m ^ d, (uint32_t)(i & 63u));
        s += UINT64_C(0xD1B54A32D192ED03);
        s *= UINT64_C(0x94D049BB133111EB);
        s ^= (s >> 29);
        d ^= s + UINT64_C(0xBF58476D1CE4E5B9) * (i + 1u);
        d = rotl64(d, (uint32_t)((i ^ (s >> 59)) & 63u));
        d *= UINT64_C(0x9E3779B185EBCA87);
        d ^= (d >> 33);

        if (yield_every != 0 && ((i + 1u) % yield_every) == 0u) {
            SwitchToThread();
        }
    }

    *out_final = s;
    *out_digest = d;
}

static DWORD WINAPI worker_main(LPVOID param) {
    WorkerArgs* a = (WorkerArgs*)param;
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);

    if (a->use_affinity) {
        DWORD cpu_count = sysinfo.dwNumberOfProcessors;
        if (cpu_count == 0) cpu_count = 1;
        uint32_t cpu = a->index % cpu_count;
        DWORD_PTR mask = ((DWORD_PTR)1u) << cpu;
        SetThreadAffinityMask(GetCurrentThread(), mask);
    }

    uint64_t seed = derive_seed(a->global_seed, a->index, a->iters);
    run_oracle_core(seed, a->iters, a->yield_every, &a->final_state, &a->digest);
    a->thread_ok = 1;
    return 0;
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

static void print_help(const char* exe) {
    printf("Usage: %s [options]\n", exe);
    printf("  --threads <n>       Worker thread count (default: 4)\n");
    printf("  --iters <count>     Iterations per worker (default: 10000000)\n");
    printf("  --seed <u64>        Global seed (default: 0x123456789ABCDEF0)\n");
    printf("  --affinity on|off   Pin each worker to a CPU (default: off)\n");
    printf("  --yield-every <n>   Call SwitchToThread every n iterations (default: 0=disabled)\n");
    printf("  --help              Show this help\n");
}

int main(int argc, char** argv) {
    uint32_t threads = 4;
    uint64_t iters = UINT64_C(10000000);
    uint64_t seed = UINT64_C(0x123456789ABCDEF0);
    int affinity = 0;
    uint64_t yield_every = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], &threads) || threads == 0) {
                fprintf(stderr, "ERROR: invalid --threads\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &iters) || iters == 0) {
                fprintf(stderr, "ERROR: invalid --iters\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &seed)) {
                fprintf(stderr, "ERROR: invalid --seed\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--affinity") == 0 && i + 1 < argc) {
            if (!parse_on_off(argv[++i], &affinity)) {
                fprintf(stderr, "ERROR: --affinity must be on|off\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--yield-every") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &yield_every)) {
                fprintf(stderr, "ERROR: invalid --yield-every\n");
                return 2;
            }
        } else {
            fprintf(stderr, "ERROR: unknown or incomplete argument: %s\n", argv[i]);
            return 2;
        }
    }

    WorkerArgs* args = (WorkerArgs*)calloc(threads, sizeof(WorkerArgs));
    HANDLE* th = (HANDLE*)calloc(threads, sizeof(HANDLE));
    if (!args || !th) {
        fprintf(stderr, "ERROR: allocation failure\n");
        free(args);
        free(th);
        return 3;
    }

    for (uint32_t t = 0; t < threads; ++t) {
        args[t].index = t;
        args[t].iters = iters;
        args[t].global_seed = seed;
        args[t].yield_every = yield_every;
        args[t].use_affinity = affinity;
        th[t] = CreateThread(NULL, 0, worker_main, &args[t], 0, NULL);
        if (!th[t]) {
            fprintf(stderr, "ERROR: CreateThread failed at worker=%u gle=%lu\n", t, GetLastError());
            for (uint32_t j = 0; j < t; ++j) {
                WaitForSingleObject(th[j], INFINITE);
                CloseHandle(th[j]);
            }
            free(args);
            free(th);
            return 4;
        }
    }

    WaitForMultipleObjects(threads, th, TRUE, INFINITE);
    for (uint32_t t = 0; t < threads; ++t) {
        CloseHandle(th[t]);
    }

    int all_pass = 1;
    for (uint32_t t = 0; t < threads; ++t) {
        uint64_t exp_final = 0, exp_digest = 0;
        uint64_t s = derive_seed(seed, t, iters);
        run_oracle_core(s, iters, yield_every, &exp_final, &exp_digest);
        int pass = args[t].thread_ok && args[t].final_state == exp_final && args[t].digest == exp_digest;
        if (!pass) all_pass = 0;
        printf("thread=%u final=0x%016" PRIx64 " digest=0x%016" PRIx64 " status=%s\n",
               t, args[t].final_state, args[t].digest, pass ? "PASS" : "FAIL");
        if (!pass) {
            printf("detail=expected final=0x%016" PRIx64 " digest=0x%016" PRIx64 "\n", exp_final, exp_digest);
            break;
        }
    }

    printf("RESULT: %s\n", all_pass ? "PASS" : "FAIL");
    free(args);
    free(th);
    return all_pass ? 0 : 1;
}
