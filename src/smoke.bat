@echo off
chcp 65001 >nul
REM SMOKE_V1: сначала запусти сервер zevvoryn.exe, потом этот файл.
REM Требования: Python 3.10+, сервер в offline-режиме, pvp=true, ops пустой.
python "%~dp0tools\smoke_bot.py" %*
if errorlevel 1 (
  echo.
  echo *** SMOKE: ЕСТЬ ПРОВАЛЕННЫЕ ТЕСТЫ ***
) else (
  echo.
  echo *** SMOKE: ВСЁ ЗЕЛЁНО ***
)
pause
