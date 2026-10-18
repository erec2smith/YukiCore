@echo off
cd /d "%~dp0"
gcc -O2 -o tools\embed.exe tools\embed.c
if errorlevel 1 (echo Build failed & exit /b 1)
tools\embed.exe src\ui.html src\ui.h
if errorlevel 1 (echo Build failed & exit /b 1)
gcc -O2 -mwindows -o YokiCore.exe src\main.c -lws2_32 -lshell32
if errorlevel 1 (echo Build failed & exit /b 1)
echo Built: YokiCore.exe
