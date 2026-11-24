@echo off
chcp 65001 >nul
set PYTHONIOENCODING=utf-8
REM ============================================================
REM  JARVIS booth launcher
REM   1. starts the brain (UDP 9001 + HTTP 8080)
REM   2. opens the HUD in Chrome kiosk mode with autoplay allowed
REM
REM  Why kiosk + the flag: browsers block audio until a click, and a
REM  closed tab means no sound at all. This removes both problems.
REM ============================================================
setlocal
cd /d "%~dp0"

echo Starting JARVIS brain...
start "JARVIS brain" /min cmd /c "python brain.py"

REM give the server a moment to bind the ports
timeout /t 2 /nobreak >nul

set CHROME="C:\Program Files\Google\Chrome\Application\chrome.exe"
if not exist %CHROME% set CHROME="C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"

echo Opening HUD...
start "" %CHROME% --kiosk --autoplay-policy=no-user-gesture-required ^
  --disable-pinch --overscroll-history-navigation=0 ^
  --user-data-dir="%TEMP%\jarvis-kiosk" ^
  http://localhost:8080

echo.
echo   UI      http://localhost:8080
echo   Close   Alt+F4 (kiosk)   /   close the brain window to stop UDP
echo.
endlocal
