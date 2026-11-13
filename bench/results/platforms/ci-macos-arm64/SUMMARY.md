# Benchmark results: Apple M1 (Virtual)

- OS: macOS-15.8.2-arm64-arm-64bit-Mach-O; 3 logical cores; commit `32edc68`
- Flags: `Release:  -O3 -DNDEBUG`
- Clock: cntvct_el0 (isb-serialised, 24.000 MHz, resolution 41.7 ns); one tick = 41.67 ns; timestamp-pair overhead 0 ns (not subtracted)
- **Latency percentiles are quantised to 41.7 ns** on this machine (its timer is coarse). Throughput comes from the wall clock over the whole loop and is not affected.
- Machine state: GitHub-hosted shared VM (macos-15), not isolated: absolute numbers and tails are unreliable; use ratios between designs. CPU load before runs: 635.9%

## Single-thread engine

| workload | Mops/s (median) | CV % | p50 ns | p99 ns | p99.9 ns | allocs/op | pinned CPU |
|---|---|---|---|---|---|---|---|
| passive | 26.51 | 2.8 | 42 | 125 | 208 | 0.000 | -1 |
| aggressive | 31.26 | 2.3 | 42 | 125 | 208 | 0.000 | -1 |
| balanced | 24.25 | 1.3 | 42 | 167 | 292 | 0.000 | -1 |

## Queue designs (producer thread -> queue -> matching thread)

- Pinning: macOS has no thread-affinity API: threads are not pinned (QoS user-interactive asks the scheduler for performance cores)
- Cross-thread timestamps: synchronised counter = True; ping-pong offset estimate -21 ns, minimum one-way hand-off 21 ns

| queue | workload | Mops/s | paced p50 ns | paced p99 ns | behind schedule | checksum = single-thread |
|---|---|---|---|---|---|---|
| mutex-book | passive | 22.67 | 83 | 333 | 0.005 | True |
| mutex-book | aggressive | 23.12 | 83 | 10143500 | 0.090 | True |
| mutex-book | balanced | 19.93 | 83 | 36125 | 0.022 | True |
| mutex-cv-queue-nocb | passive | 9.51 | 5083 | 2061620 | 0.048 | True |
| mutex-cv-queue-nocb | aggressive | 11.92 | 4333 | 40042 | 0.022 | True |
| mutex-cv-queue-nocb | balanced | 9.84 | 5042 | 89542 | 0.033 | True |
| mutex-cv-queue | passive | 9.45 | 4500 | 61167 | 0.030 | True |
| mutex-cv-queue | aggressive | 10.11 | 5125 | 469375 | 0.065 | True |
| mutex-cv-queue | balanced | 8.65 | 4375 | 28208 | 0.024 | True |
| single-thread-nocb | passive | 26.07 | 83 | 417 | 0.005 | True |
| single-thread-nocb | aggressive | 33.67 | 42 | 33500 | 0.016 | True |
| single-thread-nocb | balanced | 24.04 | 83 | 28333 | 0.031 | True |
| single-thread | passive | 25.49 | 83 | 458 | 0.006 | True |
| single-thread | aggressive | 28.62 | 83 | 313542 | 0.037 | True |
| single-thread | balanced | 23.43 | 83 | 5292 | 0.012 | True |
| spsc-backoff | passive | 18.49 | 292 | 10741000 | 0.059 | True |
| spsc-backoff | aggressive | 15.83 | 1178120 | 55592400 | 0.383 | True |
| spsc-backoff | balanced | 17.06 | 292 | 1726460 | 0.012 | True |
| spsc-nocb | passive | 18.20 | 208 | 113042 | 0.032 | True |
| spsc-nocb | aggressive | 20.74 | 167 | 6542 | 0.008 | True |
| spsc-nocb | balanced | 17.35 | 167 | 50125 | 0.017 | True |
| spsc-slot64 | passive | 16.97 | 167 | 4542 | 0.005 | True |
| spsc-slot64 | aggressive | 18.82 | 167 | 903208 | 0.022 | True |
| spsc-slot64 | balanced | 15.77 | 167 | 3167 | 0.004 | True |
| spsc-unpadded | passive | 16.47 | 208 | 5000 | 0.005 | True |
| spsc-unpadded | aggressive | 16.83 | 167 | 1230210 | 0.030 | True |
| spsc-unpadded | balanced | 15.05 | 167 | 3208 | 0.004 | True |
| spsc-yield | passive | 18.88 | 292 | 17433200 | 0.267 | True |
| spsc-yield | aggressive | 21.05 | 4004750 | 79204400 | 0.487 | True |
| spsc-yield | balanced | 13.68 | 1168000 | 65661200 | 0.373 | True |
| spsc | passive | 18.69 | 167 | 116333 | 0.010 | True |
| spsc | aggressive | 19.22 | 167 | 26125 | 0.035 | True |
| spsc | balanced | 17.30 | 167 | 7375 | 0.010 | True |

