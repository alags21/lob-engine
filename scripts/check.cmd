@echo off
rem Correctness gate: checked build (asserts on, validate() after every call), ctest, then fuzz.
rem Usage: scripts\check.cmd [fuzz_ops]
set OPS=%1
if "%OPS%"=="" set OPS=1000000
call scripts\win-build.cmd build -DCMAKE_CXX_FLAGS_RELEASE=-O2 || exit /b 1
set ENVROOT=%USERPROFILE%\miniforge3\envs\lob
set PATH=%ENVROOT%\Library\bin;%PATH%
build\tests\lob_fuzz.exe --seed 1 --ops %OPS% --runs 2 --check-every 16 || exit /b 1
build\tests\lob_fuzz.exe --seed 1 --ops 100000 --check-every 1 || exit /b 1
