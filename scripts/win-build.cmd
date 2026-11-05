@echo off
rem Windows local build: MSVC Build Tools + conda env "lob" (cmake, ninja, gtest, benchmark).
rem Usage: scripts\win-build.cmd [build_dir] [extra cmake args...]
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set ENVROOT=%USERPROFILE%\miniforge3\envs\lob
set PATH=%ENVROOT%\Library\bin;%ENVROOT%\Scripts;%PATH%
set BUILD=%1
if "%BUILD%"=="" set BUILD=build
shift
cmake -S . -B %BUILD% -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=%ENVROOT%\Library %* || exit /b 1
cmake --build %BUILD% || exit /b 1
set PATH=%ENVROOT%\Library\bin;%PATH%
ctest --test-dir %BUILD% --output-on-failure
