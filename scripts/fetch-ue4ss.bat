@echo off
REM Clone the RE-UE4SS source tree into third_party/RE-UE4SS.
REM Required before the first CMake configure of this project.

setlocal
set ROOT=%~dp0..
set DEST=%ROOT%\third_party\RE-UE4SS

if exist "%DEST%\CMakeLists.txt" (
    echo RE-UE4SS already present at %DEST%
    exit /b 0
)

echo Cloning RE-UE4SS into %DEST%...
git clone --recurse-submodules https://github.com/UE4SS-RE/RE-UE4SS "%DEST%"
if errorlevel 1 (
    echo Clone failed. Make sure git is installed and on PATH.
    exit /b 1
)
echo Done.
