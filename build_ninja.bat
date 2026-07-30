@echo off
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul 2>nul
title Zevvoryn - CMake + Ninja Release build (No RC / No Icons)

rem =====================================================================
rem  Zevvoryn build script (portable, no RC / no icon resources)
rem =====================================================================

if not "%~1"=="" (
    set "PROJECT_DIR=%~1"
) else (
    set "PROJECT_DIR=%~dp0"
)
if "!PROJECT_DIR:~-1!"=="\" set "PROJECT_DIR=!PROJECT_DIR:~0,-1!"

cd /d "!PROJECT_DIR!" 2>nul
if errorlevel 1 (
    echo.
    echo ERROR: cannot enter project folder: "!PROJECT_DIR!"
    pause
    exit /b 1
)

if not exist "!PROJECT_DIR!\CMakeLists.txt" (
    echo.
    echo ERROR: CMakeLists.txt not found in "!PROJECT_DIR!"
    pause
    exit /b 1
)

echo === Zevvoryn: CMake + Ninja Release build (No RC) ===
echo Project: !PROJECT_DIR!

rem ---------------------------------------------------------------- CMake
set "CMAKE=cmake"
where cmake >nul 2>nul
if not errorlevel 1 goto :cmake_ok

for %%P in ("C:\Program Files" "C:\Program Files (x86)") do (
    for %%E in (Community Professional Enterprise BuildTools Preview) do (
        if exist "%%~P\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
            set "CMAKE=%%~P\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            goto :cmake_ok
        )
    )
)
if exist "C:\Program Files\CMake\bin\cmake.exe" (
    set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
    goto :cmake_ok
)
if exist "%LOCALAPPDATA%\Microsoft\WinGet\Links\cmake.exe" (
    set "CMAKE=%LOCALAPPDATA%\Microsoft\WinGet\Links\cmake.exe"
    goto :cmake_ok
)
echo ERROR: CMake was not found.
pause
exit /b 1

:cmake_ok
rem ---------------------------------------------------------------- Ninja
set "NINJA=ninja"
where ninja >nul 2>nul
if not errorlevel 1 goto :ninja_ok
if exist "%LOCALAPPDATA%\Microsoft\WinGet\Links\ninja.exe" (
    set "NINJA=%LOCALAPPDATA%\Microsoft\WinGet\Links\ninja.exe"
    goto :ninja_ok
)
for %%P in ("C:\Program Files" "C:\Program Files (x86)") do (
    for %%E in (Community Professional Enterprise BuildTools Preview) do (
        if exist "%%~P\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" (
            set "NINJA=%%~P\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
            goto :ninja_ok
        )
    )
)
echo ERROR: Ninja was not found.
pause
exit /b 1

:ninja_ok
rem ------------------------------------------------- MSVC x64 environment
where cl >nul 2>nul
if not errorlevel 1 goto :vs_ready

set "VSDEVCMD="
for %%P in ("C:\Program Files" "C:\Program Files (x86)") do (
    for %%E in (Community Professional Enterprise BuildTools Preview) do (
        if exist "%%~P\Microsoft Visual Studio\2022\%%E\Common7\Tools\VsDevCmd.bat" (
            set "VSDEVCMD=%%~P\Microsoft Visual Studio\2022\%%E\Common7\Tools\VsDevCmd.bat"
            goto :vs_found
        )
    )
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "!VSWHERE!" (
    for /f "usebackq delims=" %%I in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%I\Common7\Tools\VsDevCmd.bat" set "VSDEVCMD=%%I\Common7\Tools\VsDevCmd.bat"
    )
)
if not defined VSDEVCMD (
    echo ERROR: Visual Studio 2022 C++ Build Tools were not found.
    pause
    exit /b 1
)

:vs_found
call "!VSDEVCMD!" -arch=x64 -host_arch=x64 -no_logo >nul
if errorlevel 1 (
    echo ERROR: Could not initialize MSVC environment.
    pause
    exit /b 1
)
cd /d "!PROJECT_DIR!"

:vs_ready
rem ------------------------------------------------------------ Configure
echo.
echo === Configuring Ninja Release build (ENABLE_RC=OFF / NO RES) ===
"!CMAKE!" -S . -B build-ninja -G Ninja -DCMAKE_MAKE_PROGRAM="!NINJA!" -DCMAKE_BUILD_TYPE=Release -DENABLE_RC=OFF -DENABLE_RESOURCES=OFF -DENABLE_ICON=OFF
if errorlevel 1 (
    echo CONFIGURE FAILED
    pause
    exit /b 1
)

rem ---------------------------------------------------------------- Build
echo.
echo === Building Release with Ninja ===
"!CMAKE!" --build build-ninja --parallel
if errorlevel 1 (
    echo BUILD FAILED
    pause
    exit /b 1
)

rem -------------------------------------------------- Publish executable
if not exist "!PROJECT_DIR!\release" mkdir "!PROJECT_DIR!\release"

set "EXE_SRC="
if exist "!PROJECT_DIR!\build-ninja\zevvoryn.exe" set "EXE_SRC=!PROJECT_DIR!\build-ninja\zevvoryn.exe"
if not defined EXE_SRC if exist "!PROJECT_DIR!\build-ninja\Release\zevvoryn.exe" set "EXE_SRC=!PROJECT_DIR!\build-ninja\Release\zevvoryn.exe"
if not defined EXE_SRC (
    echo ERROR: zevvoryn.exe not found in build-ninja.
    pause
    exit /b 1
)
copy /Y "!EXE_SRC!" "!PROJECT_DIR!\release\zevvoryn.exe" >nul

echo.
echo === Build Success! Launching zevvoryn.exe... ===
start "Zevvoryn" /D "!PROJECT_DIR!\release" "!PROJECT_DIR!\release\zevvoryn.exe"

endlocal
exit /b 0