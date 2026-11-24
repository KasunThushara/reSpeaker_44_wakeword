@echo off
chcp 65001 >nul
set PYTHONIOENCODING=utf-8
REM ============================================================
REM  WALL-E booth launcher
REM   1. starts the Walle brain (UDP 9001 + HTTP 8080)
REM   2. opens the HUD in Chrome kiosk mode
REM
REM  Jarvis must not be running: both use ports 8080 / 9001.
REM ============================================================
setlocal
cd /d "%~dp0"

echo Starting WALL-E brain...
start "WALL-E brain" /min cmd /c "python brain.py"

timeout /t 2 /nobreak >nul

set CHROME="C:\Program Files\Google\Chrome\Application\chrome.exe"
if not exist %CHROME% set CHROME="C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"

echo Opening HUD...
start "" %CHROME% --kiosk --autoplay-policy=no-user-gesture-required ^
  --disable-pinch --overscroll-history-navigation=0 ^
  --user-data-dir="%TEMP%\walle-kiosk" ^
  http://localhost:8080

echo.
echo   UI      http://localhost:8080
echo   Close   Alt+F4 (kiosk)   /   close the brain window to stop UDP
echo.
endlocal
