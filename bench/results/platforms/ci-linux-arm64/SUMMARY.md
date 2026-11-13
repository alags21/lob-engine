# Benchmark results: unknown

- OS: Linux-7.0.0-1017-azure-aarch64-with-glibc2.39; 4 logical cores; commit `32edc68`
- Flags: `Release:  -O3 -DNDEBUG`
- Clock: cntvct_el0 (isb-serialised, 1000.000 MHz, resolution 1.0 ns); one tick = 1.00 ns; timestamp-pair overhead 32 ns (not subtracted)
- Machine state: GitHub-hosted shared VM (ubuntu-24.04-arm), not isolated: absolute numbers and tails are unreliable; use ratios between designs. CPU load before runs: 53.5%

## Single-thread engine

| workload | Mops/s (median) | CV % | p50 ns | p99 ns | p99.9 ns | allocs/op | pinned CPU |
|---|---|---|---|---|---|---|---|
| passive | 28.09 | 0.1 | 64 | 120 | 200 | 0.000 | 1 |
| aggressive | 32.81 | 0.1 | 64 | 128 | 232 | 0.000 | 1 |
| balanced | 25.63 | 0.1 | 64 | 144 | 288 | 0.000 | 1 |

## Queue designs (producer thread -> queue -> matching thread)

- Pinning: detected 4 physical cores; used the first logical CPU of physical cores #2 and #3 (logical 2 and 3)
- Cross-thread timestamps: synchronised counter = True; ping-pong offset estimate -8 ns, minimum one-way hand-off 120 ns

| queue | workload | Mops/s | paced p50 ns | paced p99 ns | behind schedule | checksum = single-thread |
|---|---|---|---|---|---|---|
| mutex-book | passive | 19.03 | 96 | 160 | 0.000 | True |
| mutex-book | aggressive | 21.30 | 88 | 168 | 0.000 | True |
| mutex-book | balanced | 18.03 | 96 | 192 | 0.000 | True |
| mutex-cv-queue-nocb | passive | 2.69 | 3864 | 7920 | 0.001 | True |
| mutex-cv-queue-nocb | aggressive | 1.62 | 4064 | 8304 | 0.002 | True |
| mutex-cv-queue-nocb | balanced | 2.35 | 3872 | 7936 | 0.001 | True |
| mutex-cv-queue | passive | 2.78 | 3888 | 8104 | 0.002 | True |
| mutex-cv-queue | aggressive | 2.40 | 3944 | 8040 | 0.002 | True |
| mutex-cv-queue | balanced | 2.50 | 3888 | 8048 | 0.002 | True |
| single-thread-nocb | passive | 27.04 | 80 | 144 | 0.000 | True |
| single-thread-nocb | aggressive | 32.12 | 72 | 144 | 0.000 | True |
| single-thread-nocb | balanced | 25.26 | 80 | 168 | 0.000 | True |
| single-thread | passive | 26.20 | 80 | 144 | 0.001 | True |
| single-thread | aggressive | 30.51 | 72 | 152 | 0.000 | True |
| single-thread | balanced | 24.32 | 80 | 176 | 0.000 | True |
| spsc-backoff | passive | 20.31 | 456 | 792 | 0.000 | True |
| spsc-backoff | aggressive | 20.63 | 440 | 784 | 0.000 | True |
| spsc-backoff | balanced | 18.41 | 448 | 800 | 0.000 | True |
| spsc-nocb | passive | 21.82 | 272 | 408 | 0.000 | True |
| spsc-nocb | aggressive | 23.71 | 264 | 416 | 0.000 | True |
| spsc-nocb | balanced | 21.05 | 320 | 416 | 0.000 | True |
| spsc-slot64 | passive | 20.62 | 296 | 448 | 0.000 | True |
| spsc-slot64 | aggressive | 19.25 | 312 | 416 | 0.000 | True |
| spsc-slot64 | balanced | 17.51 | 280 | 456 | 0.000 | True |
| spsc-unpadded | passive | 8.44 | 384 | 504 | 0.000 | True |
| spsc-unpadded | aggressive | 8.86 | 368 | 504 | 0.000 | True |
| spsc-unpadded | balanced | 9.79 | 384 | 552 | 0.000 | True |
| spsc-yield | passive | 21.06 | 440 | 800 | 0.000 | True |
| spsc-yield | aggressive | 22.24 | 448 | 800 | 0.000 | True |
| spsc-yield | balanced | 18.40 | 448 | 800 | 0.000 | True |
| spsc | passive | 21.57 | 328 | 432 | 0.000 | True |
| spsc | aggressive | 17.66 | 272 | 432 | 0.000 | True |
| spsc | balanced | 19.44 | 288 | 432 | 0.000 | True |

