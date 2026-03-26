# C Oracles for CPU, RAM, and Mapped-File Correctness (Windows)

This repo contains three standalone Windows x64 C programs:

- `cpu_oracle.c` — deterministic multithreaded CPU-state correctness check.
- `mem_oracle.c` — RAM/memory-controller fill+verify patterns (single-thread, multi-thread, or both).
- `mmap_oracle.c` — file-backed mapped-memory write/read verification across mapping cycles.

## 1) Build on Windows 11

## Option A: Native Windows 11 build (recommended)

### Prerequisites

- Visual Studio 2022 (or Build Tools) with **Desktop development with C++**.
- Use **x64 Native Tools Command Prompt for VS 2022**.

### Build commands

```bat
cd C:\path\to\c_oracle_tests

cl /nologo /O2 /W4 /EHsc /Fe:cpu_oracle.exe cpu_oracle.c
cl /nologo /O2 /W4 /EHsc /Fe:mem_oracle.exe mem_oracle.c
cl /nologo /O2 /W4 /EHsc /Fe:mmap_oracle.exe mmap_oracle.c
```

You should now have:

- `cpu_oracle.exe`
- `mem_oracle.exe`
- `mmap_oracle.exe`

## Option B: Windows 11 + WSL workflow

Since the sources include `<windows.h>`, they are **Windows-targeted**.

You have two practical WSL approaches:

1. **Edit in WSL, build in Windows shell** (simplest):
   - Keep sources in your repo (possibly under `/mnt/c/...`), then run the same `cl` commands from PowerShell/Developer Prompt.
2. **Cross-compile in WSL using MinGW-w64**:
   - Install `mingw-w64` in WSL.
   - Build with `x86_64-w64-mingw32-gcc`:

```bash
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -o cpu_oracle.exe cpu_oracle.c
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -o mem_oracle.exe mem_oracle.c
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -o mmap_oracle.exe mmap_oracle.c
```

Then run the `.exe` from Windows (or with a compatible runtime such as Wine if installed).

## 2) Run with default settings

From Command Prompt or PowerShell in the build directory:

```bat
cpu_oracle.exe
mem_oracle.exe
mmap_oracle.exe
```

Default behavior summary:

- `cpu_oracle.exe`: `--threads 4 --iters 10000000 --seed 0x123456789ABCDEF0 --affinity off --yield-every 0`
- `mem_oracle.exe`: `--size-mb 4096 --threads 4 --seed 0xC001D00D12345678 --passes 1 --mode both --affinity off --page-random on`
- `mmap_oracle.exe`: `--size-mb 512 --seed 0x0123456789ABCDEF --cycles 1 --random-order off --flush on`

Useful help commands:

```bat
cpu_oracle.exe --help
mem_oracle.exe --help
mmap_oracle.exe --help
```

## 3) Run with parameters based on detected hardware (this environment)

I detected the current system as:

- **3 logical CPUs**
- **~17 GiB usable RAM** (`MemTotal` ≈ 18,801,884 KiB)

So a balanced starting profile is:

- CPU and memory worker threads: `3`
- RAM test allocation: `8192` MiB (8 GiB, conservative for a 17 GiB machine)
- Mmap file test size: `2048` MiB (2 GiB)

### Hardware-tuned example commands

```bat
cpu_oracle.exe --threads 3 --iters 20000000 --affinity on --yield-every 10000

mem_oracle.exe --size-mb 8192 --passes 1 --mode both --threads 3 --affinity on --page-random on --seed 1234

mmap_oracle.exe --size-mb 2048 --cycles 3 --random-order on --flush on --seed 77
```

## 4) Quick tuning rules for other systems

- Set `--threads` to your logical CPU count (or `logical_cpu_count - 1` if you need desktop responsiveness).
- Start `mem_oracle --size-mb` at about **40–60% of RAM**.
- Start `mmap_oracle --size-mb` at **10–25% of RAM**.
- Increase `--iters`, `--passes`, or `--cycles` when you want longer stress duration.

## 5) Example one-liner to discover hardware on Windows

```powershell
$cpu=(Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors; \
$ramGiB=[math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory/1GB,1); \
"Logical CPU: $cpu  RAM(GB): $ramGiB"
```

Use that output to adjust the command templates above.
