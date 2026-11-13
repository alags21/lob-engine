# Benchmark results: AMD EPYC 9V74 80-Core Processor                

- OS: Windows-2022Server-10.0.20348-SP0; 4 logical cores; commit `32edc68`
- Flags: `Release: /DWIN32 /D_WINDOWS /EHsc /O2 /Ob2 /DNDEBUG`
- Clock: rdtsc (lfence-serialised), converted with steady_clock calibration; one tick = 0.39 ns; timestamp-pair overhead 20 ns (not subtracted)
- Machine state: GitHub-hosted shared VM (windows-2022), not isolated: absolute numbers and tails are unreliable; use ratios between designs. CPU load before runs: 1.2%

## Single-thread engine

| workload | Mops/s (median) | CV % | p50 ns | p99 ns | p99.9 ns | allocs/op | pinned CPU |
|---|---|---|---|---|---|---|---|
| passive | 24.75 | 0.2 | 60 | 130 | 230 | 0.000 | 1 |
| aggressive | 27.39 | 0.3 | 60 | 140 | 250 | 0.000 | 1 |
| balanced | 21.72 | 0.4 | 70 | 170 | 341 | 0.000 | 1 |

## Queue designs (producer thread -> queue -> matching thread)

- Pinning: could not detect physical cores; used logical CPUs 0 and 1 (may be SMT siblings)
- Cross-thread timestamps: synchronised counter = True; ping-pong offset estimate 5 ns, minimum one-way hand-off 30 ns

| queue | workload | Mops/s | paced p50 ns | paced p99 ns | behind schedule | checksum = single-thread |
|---|---|---|---|---|---|---|
| mutex-book | passive | 20.82 | 99 | 248 | 0.001 | True |
| mutex-book | aggressive | 22.73 | 94 | 216 | 0.001 | True |
| mutex-book | balanced | 18.80 | 101 | 265 | 0.002 | True |
| mutex-cv-queue-nocb | passive | 5.17 | 182 | 349 | 0.001 | True |
| mutex-cv-queue-nocb | aggressive | 5.70 | 175 | 307 | 0.001 | True |
| mutex-cv-queue-nocb | balanced | 4.51 | 183 | 358 | 0.001 | True |
| mutex-cv-queue | passive | 4.59 | 183 | 351 | 0.001 | True |
| mutex-cv-queue | aggressive | 4.89 | 178 | 329 | 0.001 | True |
| mutex-cv-queue | balanced | 4.42 | 183 | 398 | 0.003 | True |
| single-thread-nocb | passive | 24.88 | 86 | 234 | 0.002 | True |
| single-thread-nocb | aggressive | 27.78 | 80 | 196 | 0.001 | True |
| single-thread-nocb | balanced | 22.10 | 88 | 248 | 0.002 | True |
| single-thread | passive | 24.14 | 87 | 238 | 0.002 | True |
| single-thread | aggressive | 26.59 | 82 | 203 | 0.001 | True |
| single-thread | balanced | 21.51 | 89 | 237 | 0.001 | True |
| spsc-backoff | passive | 16.67 | 144 | 316 | 0.003 | True |
| spsc-backoff | aggressive | 17.80 | 136 | 275 | 0.002 | True |
| spsc-backoff | balanced | 15.45 | 146 | 333 | 0.003 | True |
| spsc-nocb | passive | 17.02 | 146 | 291 | 0.001 | True |
| spsc-nocb | aggressive | 18.50 | 138 | 259 | 0.001 | True |
| spsc-nocb | balanced | 15.80 | 148 | 319 | 0.002 | True |
| spsc-slot64 | passive | 17.69 | 138 | 305 | 0.003 | True |
| spsc-slot64 | aggressive | 18.83 | 130 | 265 | 0.002 | True |
| spsc-slot64 | balanced | 16.22 | 140 | 297 | 0.001 | True |
| spsc-unpadded | passive | 16.49 | 133 | 310 | 0.003 | True |
| spsc-unpadded | aggressive | 17.67 | 124 | 264 | 0.002 | True |
| spsc-unpadded | balanced | 14.64 | 134 | 322 | 0.003 | True |
| spsc-yield | passive | 16.17 | 196 | 398 | 0.001 | True |
| spsc-yield | aggressive | 18.55 | 188 | 330 | 0.001 | True |
| spsc-yield | balanced | 15.06 | 198 | 380 | 0.001 | True |
| spsc | passive | 16.63 | 148 | 309 | 0.002 | True |
| spsc | aggressive | 17.80 | 140 | 264 | 0.001 | True |
| spsc | balanced | 14.90 | 151 | 328 | 0.003 | True |

