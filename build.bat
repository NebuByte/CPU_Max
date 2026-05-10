@echo off
REM ====================================================================
REM  Build script for CPUMax
REM    1. Compile CPUMax.exe with gcc
REM    2. Wrap in NSIS installer -> dist\CPUMax-Setup.exe
REM ====================================================================
setlocal

set OUT=CPUMax.exe
set SRC=src\cpumax.c

if not exist build mkdir build
if not exist dist  mkdir dist

REM ---- locate gcc ----------------------------------------------------
set GCC=
where gcc >nul 2>&1 && set GCC=gcc
if "%GCC%"=="" if exist "C:\Program Files\CodeBlocks\MinGW\bin\gcc.exe" (
    set "GCC=C:\Program Files\CodeBlocks\MinGW\bin\gcc.exe"
)
if "%GCC%"=="" (
    echo [build] ERROR: no gcc found. Install MinGW-w64 or CodeBlocks.
    exit /b 1
)

echo [build] compiling with "%GCC%"
"%GCC%" -O2 -Wall -mwindows -o build\%OUT% %SRC% ^
    -lpowrprof -lshell32 -luser32 -lgdi32 -ladvapi32 -lole32 -lshlwapi
if errorlevel 1 (
    echo [build] compile failed.
    exit /b 1
)
echo [build] ok -^> build\%OUT%

REM ---- locate makensis ----------------------------------------------
set NSIS=
where makensis >nul 2>&1 && set NSIS=makensis
if "%NSIS%"=="" if exist "C:\Program Files (x86)\NSIS\makensis.exe" (
    set "NSIS=C:\Program Files (x86)\NSIS\makensis.exe"
)
if "%NSIS%"=="" if exist "C:\Program Files\NSIS\makensis.exe" (
    set "NSIS=C:\Program Files\NSIS\makensis.exe"
)
if "%NSIS%"=="" (
    echo [build] WARN: makensis not found. Skipping installer.
    exit /b 0
)

echo [build] building installer with "%NSIS%"
"%NSIS%" /V2 installer\installer.nsi
if errorlevel 1 (
    echo [build] installer build failed.
    exit /b 1
)
echo [build] ok -^> dist\CPUMax-Setup.exe
exit /b 0
