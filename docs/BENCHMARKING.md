# Toolchain and benchmarking

## Local toolchain (Windows 11, no system C++ compiler needed beyond MSVC Build Tools)

`conda create -n lob -c conda-forge cmake ninja cxx-compiler gtest benchmark clang-format`
installs the build tools and libraries. On Windows, conda-forge's `cxx-compiler` is only a
wrapper (`vs2022_win-64`): it needs a real MSVC. This machine already had Visual Studio
Build Tools (MSVC 14.50, `cl.exe` under
`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools`), so that is the compiler used.

Versions installed by the command above (from `conda list -n lob`): cmake 4.4.5, gtest 1.18.0,
benchmark 1.10.0, clang-format 23.1.4.

Build and test:

```
scripts\win-build.cmd build
```

The script loads `vcvars64.bat`, puts the env's `Library\bin` on PATH, configures with Ninja in
Release, builds, and runs ctest. conda's gtest/benchmark are built against the release CRT, so
local Windows builds use Release (not Debug). Debug and sanitizer builds run in CI on Linux.

## CI toolchain
Ubuntu 24.04, gcc and clang from apt, `libgtest-dev`, `libbenchmark-dev`, Ninja, CMake from apt.

## Benchmark methodology
Filled in with Stage 3. Every result file must record OS, CPU, compiler, flags, commit SHA, run
count, and whether turbo/background load was controlled.
