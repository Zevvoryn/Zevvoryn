@echo off
chcp 65001 >nul
title NetherCraft Server Setup
color 0A

echo.
echo  ╔══════════════════════════════════════════════╗
echo  ║       NETHERCRAFT SERVER - SETUP WIZARD      ║
echo  ║       Minecraft Java Edition 1.21.1          ║
echo  ║       C++20 • No Java • No Wrappers          ║
echo  ╚══════════════════════════════════════════════╝
echo.

:: ============================================
:: Определяем директорию установки
:: ============================================
set "INSTALL_DIR=%~dp0"
cd /d "%INSTALL_DIR%"

:: ============================================
:: Проверяем наличие серверного бинарника
:: ============================================
if not exist "nethercraft.exe" (
    echo [ОШИБКА] nethercraft.exe не найден!
    echo Поместите nethercraft.exe в эту папку и запустите setup.bat снова.
    echo.
    pause
    exit /b 1
)

:: ============================================
:: Создаём структуру директорий
:: ============================================
echo [1/6] Создание структуры директорий...
if not exist "world" mkdir world
if not exist "logs" mkdir logs
if not exist "players" mkdir players
if not exist "plugins" mkdir plugins
echo       Готово.
echo.

:: ============================================
:: Язык
:: ============================================
echo [2/6] Выбор языка консоли:
echo.
echo   1) Русский
echo   2) English
echo.
set /p "LANG_CHOICE=Выберите язык [1]: "
if "%LANG_CHOICE%"=="" set "LANG_CHOICE=1"
if "%LANG_CHOICE%"=="1" (
    set "LANG_NAME=rus"
    set "MOTD_DEFAULT=NetherCraft Сервер"
    set "WELCOME=Добро пожаловать на NetherCraft!"
) else (
    set "LANG_NAME=eng"
    set "MOTD_DEFAULT=NetherCraft Server"
    set "WELCOME=Welcome to NetherCraft!"
)
echo.

:: ============================================
:: Основные настройки
:: ============================================
echo [3/6] Основные настройки сервера:
echo.
set /p "SERVER_NAME=Имя сервера [%MOTD_DEFAULT%]: "
if "%SERVER_NAME%"=="" set "SERVER_NAME=%MOTD_DEFAULT%"
set /p "PORT=Порт [25565]: "
if "%PORT%"=="" set "PORT=25565"
set /p "MAX_PLAYERS=Макс. игроков [20]: "
if "%MAX_PLAYERS%"=="" set "MAX_PLAYERS=20"
set /p "VIEW_DISTANCE=Дальность прорисовки [10]: "
if "%VIEW_DISTANCE%"=="" set "VIEW_DISTANCE=10"
echo.

:: ============================================
:: Настройки мира
:: ============================================
echo [4/6] Настройки мира:
echo.
echo   Тип мира:
echo   1) flat (плоский)
echo   2) normal (в будущем)
echo.
set /p "WORLD_TYPE=Тип мира [1]: "
if "%WORLD_TYPE%"=="" set "WORLD_TYPE=1"
if "%WORLD_TYPE%"=="1" (
    set "WORLD_TYPE_NAME=flat"
) else (
    set "WORLD_TYPE_NAME=flat"
)
set /p "WORLD_SEED=Семя мира (пусто = случайное): "
echo.

:: ============================================
:: Режим игры
:: ============================================
echo [5/6] Настройки геймплея:
echo.
echo   Режим по умолчанию:
echo   1) survival  (выживание)
echo   2) creative  (креатив)
echo   3) adventure (приключение)
echo   4) spectator (наблюдатель)
echo.
set /p "GAMEMODE=Режим игры [2]: "
if "%GAMEMODE%"=="" set "GAMEMODE=2"
if "%GAMEMODE%"=="1" set "GAMEMODE_NAME=survival"
if "%GAMEMODE%"=="2" set "GAMEMODE_NAME=creative"
if "%GAMEMODE%"=="3" set "GAMEMODE_NAME=adventure"
if "%GAMEMODE%"=="4" set "GAMEMODE_NAME=spectator"
set /p "HARDCORE=Хардкор? (y/n) [n]: "
if "%HARDCORE%"=="" set "HARDCORE=false"
if /i "%HARDCORE%"=="y" set "HARDCORE=true"
set /p "ONLINE_MODE=Онлайн-режим (требует Mojang) (y/n) [y]: "
if "%ONLINE_MODE%"=="" set "ONLINE_MODE=true"
if /i "%ONLINE_MODE%"=="n" set "ONLINE_MODE=false"
set /p "PVP=PvP (y/n) [y]: "
if "%PVP%"=="" set "PVP=true"
if /i "%PVP%"=="n" set "PVP=false"
echo.

:: ============================================
:: Продвинутые настройки
:: ============================================
echo [6/6] Продвинутые настройки:
echo.
set /p "COMPRESSION=Порог сжатия пакетов [256]: "
if "%COMPRESSION%"=="" set "COMPRESSION=256"
echo.

:: ============================================
:: Генерация server.json
:: ============================================
echo ============================================
echo  Генерация конфигурации server.json...
echo ============================================
echo.

(
echo {
echo     "server_name": "%SERVER_NAME%",
echo     "motd": "%SERVER_NAME%",
echo     "max_players": %MAX_PLAYERS%,
echo     "port": %PORT%,
echo     "view_distance": %VIEW_DISTANCE%,
echo     "online_mode": %ONLINE_MODE%,
echo     "compression_threshold": %COMPRESSION%,
echo     "world_type": "%WORLD_TYPE_NAME%",
echo     "world_seed": 0,
echo     "gamemode": "%GAMEMODE_NAME%",
echo     "hardcore": %HARDCORE%,
echo     "pvp": %PVP%,
echo     "language": "%LANG_NAME%",
echo     "spawn_protection": 16,
echo     "difficulty": 2,
echo     "auto_save": true,
echo     "auto_save_interval": 300,
echo     "view_distance_chunks": 10,
echo     "simulation_distance": 8
echo }
) > server.json

:: Генерируем server.properties (совместимость)
(
echo # NetherCraft Server Properties
echo motd=%SERVER_NAME%
echo server-port=%PORT%
echo max-players=%MAX_PLAYERS%
echo view-distance=%VIEW_DISTANCE%
echo level-seed=0
echo gamemode=%GAMEMODE_NAME%
echo hardcore=%HARDCORE%
echo pvp=%PVP%
echo online-mode=%ONLINE_MODE%
echo difficulty=2
echo level-name=world
echo spawn-protection=16
echo auto-save=true
echo language=%LANG_NAME%
) > server.properties

echo [OK] server.json создан
echo [OK] server.properties создан
echo.

:: ============================================
:: Готово
:: ============================================
echo ╔══════════════════════════════════════════════╗
echo ║            УСТАНОВКА ЗАВЕРШЕНА!              ║
echo ╠══════════════════════════════════════════════╣
echo ║  Сервер: %SERVER_NAME%
echo ║  Порт:   %PORT%
echo ║  Мир:    %WORLD_TYPE_NAME%
echo ║  Режим:  %GAMEMODE_NAME%
echo ║  Игроки: %MAX_PLAYERS% макс.
echo ╠══════════════════════════════════════════════╣
echo ║  Запуск:双击 start.bat или start.cmd         ║
echo ╚══════════════════════════════════════════════╝
echo.
echo Нажмите любую клавишу для запуска сервера...
pause >nul

:: Запускаем сервер
call start.bat
