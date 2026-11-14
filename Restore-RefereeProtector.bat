@echo off
rem Reverts Install-PreserveLoader.bat: moves preserve\Referee.dll back to Referee.dll and
rem restores 2025Patch.dll to the game root (so the classic Injector flow works again).
rem Other modules you added under preserve\modules\ are left untouched on purpose.
setlocal
cd /d "%~dp0"

if not exist "preserve\Referee.dll" (
    echo [!] preserve\Referee.dll not found; nothing to restore.
    pause
    exit /b 1
)
tasklist /fi "imagename eq Recroom_Release.exe" | find /i "Recroom_Release.exe" >nul && (
    echo [!] Rec Room is running. Close the game first.
    pause
    exit /b 1
)

move /y "preserve\Referee.dll" "Referee.dll" >nul || (
    echo [!] Could not move the protector back to Referee.dll.
    pause
    exit /b 1
)
if exist "preserve\modules\2025Patch.dll" move /y "preserve\modules\2025Patch.dll" "2025Patch.dll" >nul
echo [OK] The protector is back at Referee.dll.
echo      PreserveLoader.dll is still in this folder; the game no longer uses it.
echo      Any extra modules under preserve\modules\ were left alone.
