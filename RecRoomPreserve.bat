@echo off
rem Launches Rec Room in screen (desktop) mode straight through the game's own import
rem binding: Referee.dll is the preservation loader, which attaches every DLL in
rem preserve\modules\ on its own shortly after GameAssembly.dll appears.
rem No injector window, no remote threads.
rem
rem Requires:
rem   - Steam running (the game will not boot without it),
rem   - Install-PreserveLoader.bat already run in this folder.
setlocal
cd /d "%~dp0"

if not exist "preserve\Referee.dll" (
    echo [!] preserve\Referee.dll not found. Run Install-PreserveLoader.bat first.
    pause
    exit /b 1
)
if not exist "Recroom_Release.exe" (
    echo [!] Recroom_Release.exe not found next to this script.
    pause
    exit /b 1
)

echo Launching Rec Room (screen) via PreserveLoader...
start "" "Recroom_Release.exe" +forcemode:screen
