@echo off
cd /d "%~dp0.."

if not exist "build\gui\Release\audiosub_gui.exe" (
    echo GUI not found. Run scripts\build.ps1 first.
    exit /b 1
)

echo Starting signaling server...
start "AudioSub Signaling" cmd /k python signaling\server.py

timeout /t 2 /nobreak >nul

echo Starting GUI B...
start "" build\gui\Release\audiosub_gui.exe --id B

timeout /t 1 /nobreak >nul

echo Starting GUI A...
start "" build\gui\Release\audiosub_gui.exe --id A

echo.
echo Done. Whisper model load takes ~10s before the window appears.
echo Close the "AudioSub Signaling" window to stop the server.
