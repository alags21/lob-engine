// Platform layer shared by the benchmark binaries (lob_bench, lob_mt_bench, lob_itch bench).
//
// Everything that differs between x86-64 and arm64, or between Windows, Linux and macOS, lives here:
//   * the per-operation timestamp (`cycles`) and how fast it ticks (`ticks_per_ns`, `clock_name`);
//   * the spin-wait hint (`cpu_relax`);
//   * the CPU model string and thread pinning.
//
// x86-64: lfence-serialised RDTSC, calibrated against steady_clock.
// arm64:  isb-serialised read of the architected virtual counter (CNTVCT_EL0). Its frequency is
//         reported by CNTFRQ_EL0 and is 24 MHz on Apple M-series, so one tick is about 41.7 ns:
//         per-operation latencies are quantised to that step, while throughput (wall clock over
//         the whole loop) is not affected. `clock_name()` states the resolution so it ends up in
//         every result file.
// macOS has no thread-affinity API; `pin_thread` only raises the QoS class (which asks the scheduler
// for performance cores) and returns false, so result files record the threads as not pinned.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/sysctl.h>
#endif

#if defined(_M_X64) || defined(__x86_64__)
#define LOB_HAS_TSC 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#include <x86intrin.h>
#endif
#elif (defined(__aarch64__) || defined(__arm64__)) && !defined(_MSC_VER)
#define LOB_HAS_CNTVCT 1
#endif

namespace lob::bench {

/// A timestamp in clock ticks. Compare only against other `cycles()` values.
inline std::uint64_t cycles() {
#if defined(LOB_HAS_TSC)
  _mm_lfence();
  const std::uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
#elif defined(LOB_HAS_CNTVCT)
  std::uint64_t t;
  __asm__ volatile("isb\n\tmrs %0, cntvct_el0\n\tisb" : "=r"(t)::"memory");
  return t;
#else
  return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

/// Hint for spin-wait loops.
inline void cpu_relax() {
#if defined(LOB_HAS_TSC)
  _mm_pause();
#elif defined(LOB_HAS_CNTVCT)
  __asm__ volatile("yield" ::: "memory");
#else
  std::this_thread::yield();
#endif
}

#if defined(LOB_HAS_CNTVCT)
inline double counter_hz() {
  std::uint64_t f;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
  return static_cast<double>(f);
}
#endif

/// Ticks of `cycles()` per nanosecond. x86: measured over 200 ms against steady_clock. arm64: read
/// from the counter's frequency register. Otherwise the fallback clock ticks in nanoseconds.
inline double ticks_per_ns() {
#if defined(LOB_HAS_TSC)
  using clk = std::chrono::steady_clock;
  const auto t0 = clk::now();
  const std::uint64_t a = cycles();
  while (clk::now() - t0 < std::chrono::milliseconds(200)) {
  }
  const std::uint64_t b = cycles();
  const double ns = std::chrono::duration<double, std::nano>(clk::now() - t0).count();
  return static_cast<double>(b - a) / ns;
#elif defined(LOB_HAS_CNTVCT)
  return counter_hz() / 1e9;
#else
  return 1.0;
#endif
}

/// Human-readable description of the timestamp source, written into every result file.
inline std::string clock_name() {
#if defined(LOB_HAS_TSC)
  return "rdtsc (lfence-serialised)";
#elif defined(LOB_HAS_CNTVCT)
  char buf[128];
  std::snprintf(buf, sizeof buf, "cntvct_el0 (isb-serialised, %.3f MHz, resolution %.1f ns)", counter_hz() / 1e6,
                1e9 / counter_hz());
  return buf;
#else
  return "steady_clock (ns ticks)";
#endif
}

/// True when `ticks_per_ns` comes from a calibration against steady_clock (x86) rather than a register.
inline constexpr bool kClockIsCalibrated =
#if defined(LOB_HAS_TSC)
    true;
#else
    false;
#endif

/// True when timestamps taken on different cores are on one timeline: x86 invariant-TSC bit, or the
/// architected system counter on arm64.
inline bool counter_is_synchronised() {
#if defined(LOB_HAS_TSC)
  int r[4] = {0};
#if defined(_MSC_VER)
  __cpuid(r, 0x80000007);
#else
  if (!__get_cpuid(0x80000007u, reinterpret_cast<unsigned*>(&r[0]), reinterpret_cast<unsigned*>(&r[1]),
                   reinterpret_cast<unsigned*>(&r[2]), reinterpret_cast<unsigned*>(&r[3])))
    return false;
#endif
  return (r[3] >> 8) & 1;  // EDX bit 8: invariant TSC
#elif defined(LOB_HAS_CNTVCT)
  return true;
#else
  return false;
#endif
}

inline std::string cpu_brand() {
#if defined(LOB_HAS_TSC)
  int r[4] = {0};
  char buf[49] = {0};
  for (unsigned i = 0; i < 3; ++i) {
#if defined(_MSC_VER)
    __cpuid(r, static_cast<int>(0x80000002u + i));
#else
    __cpuid(0x80000002u + i, r[0], r[1], r[2], r[3]);
#endif
    std::memcpy(buf + 16 * i, r, 16);
  }
  std::string s(buf);
  const auto p = s.find_first_not_of(' ');
  return p == std::string::npos ? s : s.substr(p);
#elif defined(__APPLE__)
  char buf[128] = {0};
  std::size_t n = sizeof buf;
  if (sysctlbyname("machdep.cpu.brand_string", buf, &n, nullptr, 0) == 0) return buf;
  return "unknown";
#elif defined(__linux__)
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("model name", 0) == 0 || line.rfind("Model", 0) == 0) {
      const auto c = line.find(':');
      if (c != std::string::npos) return line.substr(c + 2);
    }
  }
  return "unknown";
#else
  return "unknown";
#endif
}

/// Pins the calling thread to logical CPU `cpu` and raises its priority. Returns true only if the
/// thread is now pinned (never true on macOS, which has no affinity API).
inline bool pin_thread(int cpu) {
#if defined(_WIN32)
  SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  return cpu >= 0 && SetThreadAffinityMask(GetCurrentThread(), 1ULL << cpu) != 0;
#elif defined(__linux__)
  if (cpu < 0) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return sched_setaffinity(0, sizeof(set), &set) == 0;
#elif defined(__APPLE__)
  (void)cpu;
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  return false;
#else
  (void)cpu;
  return false;
#endif
}

}  // namespace lob::bench
