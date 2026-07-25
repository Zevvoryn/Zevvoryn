@echo off
chcp 65001 >nul
REM STRESS_V1: сначала запусти сервер zevvoryn.exe, потом этот файл.
REM 100 ботов на 2 минуты. Параметры: stress.bat --bots 50 --duration 60
python "%~dp0tools\stress_bot.py" %*
pause
