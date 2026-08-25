@echo off
rem One-click launcher: starts the injector, then Rec Room in VR mode.
rem
rem Drop this into the Rec Room folder next to Recroom_Release.exe, together with
rem Injector.exe, 2025Patch.dll and 2025patch.ini -- everything is resolved relative
rem to this script, so the whole release zip works unpacked in place.
rem
rem The injector starts first on purpose: it waits for Recroom_Release.exe, then
rem until both GameAssembly.dll and Referee.dll are loaded, and only then attaches
rem (attaching earlier crashes -- the patch resolves against both at attach time).
rem It skips instances that already carry the patch, so running this again while the
rem game runs is safe -- and if you want a SECOND client, just run this again: the
rem injector patches the newly launched one instead of stopping at the running one.
rem
rem The patch has no console window by default: Unity throttles hard whenever the
rem game loses focus, which measurably hurts room-load times. Confirm it attached
rem by reading 2025patch.log in this folder, or set EnableConsole=true in
rem 2025patch.ini if you want the window back.
setlocal
cd /d "%~dp0"

if not exist "Injector.exe" (
    echo [!] Injector.exe not found next to this script.
    echo     Unpack 2025Patch.dll and Injector.exe into the Rec Room folder.
    pause
    exit /b 1
)
if not exist "Recroom_Release.exe" (
    echo [!] Recroom_Release.exe not found next to this script.
    echo     Put this file in the Rec Room install folder.
    pause
    exit /b 1
)

echo Starting injector (it waits for the game)...
start "2025 patch injector" "Injector.exe"

echo Launching Rec Room (vr)...
start "" "Recroom_Release.exe" +forcemode:vr
