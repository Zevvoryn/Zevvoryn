@echo off
chcp 65001 >nul
title NetherCraft Server
color 0B

set "INSTALL_DIR=%~dp0"
cd /d "%INSTALL_DIR%"

echo.
echo  ╔══════════════════════════════════════╗
echo  ║     NETHERCRAFT SERVER - 1.21.1      ║
echo  ╚══════════════════════════════════════╝
echo.

if not exist "nethercraft.exe" (
    echo [ОШИБКА] nethercraft.exe не найден!
    pause
    exit /b 1
)

if not exist "server.json" (
    echo [ОШИБКА] server.json не найден!
    echo Запустите setup.bat для настройки сервера.
    pause
    exit /b 1
)

:: Запуск сервера
echo Запуск NetherCraft...
echo.
nethercraft.exe server.json
echo.
echo Сервер остановлен.
pause
