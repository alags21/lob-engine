# Benchmark results: Intel(R) Core(TM) i7-8700B CPU @ 3.20GHz

- OS: macOS-15.7.9-x86_64-i386-64bit-Mach-O; 4 logical cores; commit `32edc68`
- Flags: `Release:  -O3 -DNDEBUG`
- Clock: rdtsc (lfence-serialised), converted with steady_clock calibration; one tick = 0.31 ns; timestamp-pair overhead 12 ns (not subtracted)
- Machine state: GitHub-hosted shared VM (macos-15-intel), not isolated: absolute numbers and tails are unreliable; use ratios between designs. CPU load before runs: 125.5%

## Single-thread engine

| workload | Mops/s (median) | CV % | p50 ns | p99 ns | p99.9 ns | allocs/op | pinned CPU |
|---|---|---|---|---|---|---|---|
| passive | 17.79 | 2.5 | 70 | 169 | 313 | 0.000 | -1 |
| aggressive | 20.32 | 3.9 | 63 | 184 | 347 | 0.000 | -1 |
| balanced | 14.75 | 3.5 | 76 | 219 | 461 | 0.000 | -1 |

## Queue designs (producer thread -> queue -> matching thread)

- Pinning: macOS has no thread-affinity API: threads are not pinned (QoS user-interactive asks the scheduler for performance cores)
- Cross-thread timestamps: synchronised counter = False; ping-pong offset estimate -19 ns, minimum one-way hand-off 61 ns

| queue | workload | Mops/s | paced p50 ns | paced p99 ns | behind schedule | checksum = single-thread |
|---|---|---|---|---|---|---|
| mutex-book | passive | 8.99 | 211 | 798954 | 0.052 | True |
| mutex-book | aggressive | 11.18 | 149 | 533 | 0.005 | True |
| mutex-book | balanced | 8.87 | 191 | 9311 | 0.015 | True |
| mutex-cv-queue-nocb | passive | 3.39 | 93483 | 7315400 | 0.502 | True |
| mutex-cv-queue-nocb | aggressive | 3.83 | 79195 | 5637470 | 0.510 | True |
| mutex-cv-queue-nocb | balanced | 3.14 | 85053 | 6738990 | 0.516 | True |
| mutex-cv-queue | passive | 1.82 | 125760 | 22164900 | 0.576 | True |
| mutex-cv-queue | aggressive | 4.41 | 57910 | 4367180 | 0.515 | True |
| mutex-cv-queue | balanced | 3.13 | 93804 | 7084720 | 0.516 | True |
| single-thread-nocb | passive | 12.93 | 161 | 4024 | 0.010 | True |
| single-thread-nocb | aggressive | 16.02 | 124 | 7015 | 0.011 | True |
| single-thread-nocb | balanced | 11.48 | 160 | 8583 | 0.015 | True |
| single-thread | passive | 14.08 | 149 | 9505 | 0.016 | True |
| single-thread | aggressive | 17.67 | 124 | 4575 | 0.011 | True |
| single-thread | balanced | 14.35 | 136 | 1860 | 0.009 | True |
| spsc-backoff | passive | 6.57 | 389 | 49522 | 0.011 | True |
| spsc-backoff | aggressive | 6.92 | 348 | 26109 | 0.009 | True |
| spsc-backoff | balanced | 6.69 | 324 | 2446560 | 0.056 | True |
| spsc-nocb | passive | 8.19 | 336 | 18195 | 0.009 | True |
| spsc-nocb | aggressive | 10.39 | 279 | 8249 | 0.007 | True |
| spsc-nocb | balanced | 7.17 | 344 | 137893 | 0.028 | True |
| spsc-slot64 | passive | 7.55 | 379 | 29452 | 0.018 | True |
| spsc-slot64 | aggressive | 9.50 | 338 | 20666 | 0.010 | True |
| spsc-slot64 | balanced | 7.18 | 371 | 128853 | 0.025 | True |
| spsc-unpadded | passive | 5.50 | 433 | 30766 | 0.014 | True |
| spsc-unpadded | aggressive | 5.97 | 386 | 30728 | 0.016 | True |
| spsc-unpadded | balanced | 6.10 | 371 | 125072 | 0.027 | True |
| spsc-yield | passive | 8.38 | 612 | 183418 | 0.008 | True |
| spsc-yield | aggressive | 8.00 | 611 | 64438 | 0.009 | True |
| spsc-yield | balanced | 7.88 | 621 | 896552 | 0.060 | True |
| spsc | passive | 6.22 | 361 | 1961150 | 0.103 | True |
| spsc | aggressive | 7.75 | 273 | 6690 | 0.007 | True |
| spsc | balanced | 6.79 | 352 | 270685 | 0.035 | True |

