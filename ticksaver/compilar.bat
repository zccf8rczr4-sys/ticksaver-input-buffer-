@echo off
title TickSaver - compilador
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0compilar.ps1"
echo.
echo Podes cerrar esta ventana.
pause
