@echo off
setlocal EnableExtensions
cd /d "%~dp0"

echo === Zevvoryn: CMake + Ninja Release build ===

rem ---- Locate CMake ----
set "CMAKE=cmake"
where cmake >nul 2>nul
if not errorlevel 1 goto :cmake_ok

for %%E in (Community Professional Enterprise BuildTools) do (
    if exist "C:\Program Files\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
        set "CMAKE=C:\Program Files\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        goto :cmake_ok
    )
)
if exist "C:\Program Files\CMake\bin\cmake.exe" (
    set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
    goto :cmake_ok
)
echo.
echo ERROR: CMake was not found.
echo Install it with: winget install Kitware.CMake
pause
exit /b 1

:cmake_ok
rem ---- Locate Ninja ----
set "NINJA=ninja"
where ninja >nul 2>nul
if not errorlevel 1 goto :ninja_ok
if exist "%LOCALAPPDATA%\Microsoft\WinGet\Links\ninja.exe" (
    set "NINJA=%LOCALAPPDATA%\Microsoft\WinGet\Links\ninja.exe"
    goto :ninja_ok
)
echo.
echo ERROR: Ninja was not found.
echo Install it with: winget install Ninja-build.Ninja
echo Then close and reopen this terminal, and run this file again.
pause
exit /b 1

:ninja_ok
rem ---- Load MSVC x64 build environment for Ninja ----
set "VSDEVCMD="
for %%E in (Community Professional Enterprise BuildTools) do (
    if exist "C:\Program Files\Microsoft Visual Studio\2022\%%E\Common7\Tools\VsDevCmd.bat" (
        set "VSDEVCMD=C:\Program Files\Microsoft Visual Studio\2022\%%E\Common7\Tools\VsDevCmd.bat"
        goto :vs_ok
    )
)
echo.
echo ERROR: Visual Studio 2022 C++ Build Tools were not found.
echo Install the Desktop development with C++ workload in Visual Studio Installer.
pause
exit /b 1

:vs_ok
call "%VSDEVCMD%" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 (
    echo ERROR: Could not initialize the MSVC x64 environment.
    pause
    exit /b 1
)

rem ---- Configure: safe after deleting CMakeCache.txt ----
echo.
echo === Configuring Ninja Release build ===
"%CMAKE%" -S . -B build-ninja -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo.
    echo CONFIGURE FAILED
    pause
    exit /b 1
)

rem ---- Build ----
echo.
echo === Building Release with Ninja ===
"%CMAKE%" --build build-ninja --parallel
if errorlevel 1 (
    echo.
    echo BUILD FAILED
    pause
    exit /b 1
)

rem ---- Publish executable ----
if not exist release mkdir release
if not exist "build-ninja\zevvoryn.exe" (
    echo.
    echo ERROR: Build completed but build-ninja\zevvoryn.exe was not found.
    pause
    exit /b 1
)
copy /Y "build-ninja\zevvoryn.exe" "release\zevvoryn.exe" >nul

echo.
echo === Done: release\zevvoryn.exe ===
pause
endlocal
