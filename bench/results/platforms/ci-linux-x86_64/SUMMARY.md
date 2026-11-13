# Benchmark results: AMD EPYC 7763 64-Core Processor                

- OS: Linux-7.0.0-1017-azure-x86_64-with-glibc2.39; 4 logical cores; commit `32edc68`
- Flags: `Release:  -O3 -DNDEBUG`
- Clock: rdtsc (lfence-serialised), converted with steady_clock calibration; one tick = 0.41 ns; timestamp-pair overhead 20 ns (not subtracted)
- Machine state: GitHub-hosted shared VM (ubuntu-24.04), not isolated: absolute numbers and tails are unreliable; use ratios between designs. CPU load before runs: 61.6%

## Single-thread engine

| workload | Mops/s (median) | CV % | p50 ns | p99 ns | p99.9 ns | allocs/op | pinned CPU |
|---|---|---|---|---|---|---|---|
| passive | 23.53 | 1.0 | 70 | 130 | 240 | 0.000 | 1 |
| aggressive | 27.98 | 0.3 | 60 | 140 | 270 | 0.000 | 1 |
| balanced | 21.50 | 0.4 | 70 | 171 | 361 | 0.000 | 1 |

## Queue designs (producer thread -> queue -> matching thread)

- Pinning: could not detect physical cores; used logical CPUs 0 and 1 (may be SMT siblings)
- Cross-thread timestamps: synchronised counter = True; ping-pong offset estimate -33 ns, minimum one-way hand-off 30 ns

| queue | workload | Mops/s | paced p50 ns | paced p99 ns | behind schedule | checksum = single-thread |
|---|---|---|---|---|---|---|
| mutex-book | passive | 19.33 | 110 | 1135 | 0.006 | True |
| mutex-book | aggressive | 21.78 | 98 | 1067 | 0.006 | True |
| mutex-book | balanced | 17.86 | 110 | 1122 | 0.006 | True |
| mutex-cv-queue-nocb | passive | 4.03 | 3929 | 14618 | 0.021 | True |
| mutex-cv-queue-nocb | aggressive | 4.69 | 4003 | 14601 | 0.023 | True |
| mutex-cv-queue-nocb | balanced | 4.22 | 3881 | 14979 | 0.016 | True |
| mutex-cv-queue | passive | 3.97 | 3857 | 14666 | 0.014 | True |
| mutex-cv-queue | aggressive | 4.55 | 4153 | 14731 | 0.028 | True |
| mutex-cv-queue | balanced | 4.14 | 4127 | 14789 | 0.030 | True |
| single-thread-nocb | passive | 23.56 | 101 | 1098 | 0.006 | True |
| single-thread-nocb | aggressive | 27.70 | 89 | 1074 | 0.006 | True |
| single-thread-nocb | balanced | 21.57 | 100 | 1078 | 0.006 | True |
| single-thread | passive | 22.73 | 103 | 1144 | 0.006 | True |
| single-thread | aggressive | 26.13 | 91 | 1075 | 0.006 | True |
| single-thread | balanced | 20.74 | 102 | 1109 | 0.006 | True |
| spsc-backoff | passive | 13.41 | 141 | 5281 | 0.009 | True |
| spsc-backoff | aggressive | 15.79 | 127 | 5547 | 0.009 | True |
| spsc-backoff | balanced | 12.96 | 139 | 5469 | 0.009 | True |
| spsc-nocb | passive | 13.79 | 135 | 5312 | 0.010 | True |
| spsc-nocb | aggressive | 16.99 | 120 | 5010 | 0.009 | True |
| spsc-nocb | balanced | 13.57 | 134 | 5541 | 0.010 | True |
| spsc-slot64 | passive | 14.26 | 135 | 5222 | 0.009 | True |
| spsc-slot64 | aggressive | 16.66 | 121 | 5531 | 0.009 | True |
| spsc-slot64 | balanced | 13.72 | 133 | 5243 | 0.009 | True |
| spsc-unpadded | passive | 12.93 | 132 | 4908 | 0.009 | True |
| spsc-unpadded | aggressive | 15.23 | 119 | 5649 | 0.010 | True |
| spsc-unpadded | balanced | 12.67 | 131 | 5107 | 0.009 | True |
| spsc-yield | passive | 14.46 | 549 | 6392 | 0.010 | True |
| spsc-yield | aggressive | 16.89 | 534 | 6106 | 0.009 | True |
| spsc-yield | balanced | 13.67 | 549 | 6131 | 0.009 | True |
| spsc | passive | 13.35 | 137 | 5604 | 0.009 | True |
| spsc | aggressive | 15.81 | 124 | 4932 | 0.009 | True |
| spsc | balanced | 13.06 | 137 | 5443 | 0.009 | True |

