@echo off
REM Copy Tolk + dependency DLLs into Palworld's Win64 directory so the running
REM game process can resolve them. Run this once after installing UE4SS.
REM
REM Usage:
REM   install-runtime.bat "C:\Program Files (x86)\Steam\steamapps\common\Palworld"

setlocal
set ROOT=%~dp0..
set GAME=%~1

if "%GAME%"=="" (
    set GAME=C:\Program Files ^(x86^)\Steam\steamapps\common\Palworld
)

set TARGET=%GAME%\Pal\Binaries\Win64
if not exist "%TARGET%\Palworld-Win64-Shipping.exe" (
    echo Cannot find Palworld-Win64-Shipping.exe in %TARGET%
    echo Pass your Palworld install path as the first argument.
    exit /b 1
)

echo Copying Tolk runtime DLLs to %TARGET% ...
copy /Y "%ROOT%\third_party\tolk\Tolk.dll"                    "%TARGET%\Tolk.dll"                    >nul
copy /Y "%ROOT%\third_party\tolk\nvdaControllerClient64.dll"  "%TARGET%\nvdaControllerClient64.dll"  >nul
copy /Y "%ROOT%\third_party\tolk\SAAPI64.dll"                 "%TARGET%\SAAPI64.dll"                 >nul

echo Creating mod folder skeleton ...
if not exist "%TARGET%\Mods\PalAccessibility\dlls"    mkdir "%TARGET%\Mods\PalAccessibility\dlls"
if not exist "%TARGET%\Mods\PalAccessibility\Scripts" mkdir "%TARGET%\Mods\PalAccessibility\Scripts"
copy /Y "%ROOT%\Mods\PalAccessibility\enabled.txt"        "%TARGET%\Mods\PalAccessibility\enabled.txt"        >nul
copy /Y "%ROOT%\Mods\PalAccessibility\Scripts\main.lua"   "%TARGET%\Mods\PalAccessibility\Scripts\main.lua"   >nul

echo.
echo Done. Build the mod DLL (see BUILD.md) and it will be copied to:
echo   %TARGET%\Mods\PalAccessibility\dlls\main.dll
echo.
echo Don't forget to add a line "PalAccessibility : 1" to
echo   %TARGET%\Mods\mods.txt
echo (UE4SS will create that file on first run if missing.)
