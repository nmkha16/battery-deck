@echo off
rem Builds build\BatteryDeck.exe with LLVM-MinGW (clang). Use build.bat for Visual Studio instead.
setlocal
cd /d "%~dp0"

rem Find clang++: on PATH, or in the winget install of LLVM-MinGW.
set "CXX="
for %%i in (clang++.exe) do set "CXX=%%~$PATH:i"
if not defined CXX (
    for /d %%d in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW*") do (
        for /d %%e in ("%%d\llvm-mingw*") do set "TOOLS=%%e\bin"
    )
)
if not defined CXX if defined TOOLS set "CXX=%TOOLS%\clang++.exe"
if not defined CXX (
    echo clang++ not found. Install LLVM-MinGW or add it to PATH.
    exit /b 1
)
for %%i in ("%CXX%") do set "BIN=%%~dpi"

if not exist build mkdir build

"%BIN%windres.exe" -I src res\app.rc -O coff -o build\app.res.o || exit /b 1

rem -static: no runtime DLLs to ship. -municode: wWinMain entry point. -mwindows: no console.
"%CXX%" -std=c++20 -O2 -s -municode -mwindows -static ^
    -ffunction-sections -fdata-sections -Wl,--gc-sections ^
    -Wall -Wextra -Wno-missing-field-initializers ^
    -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 ^
    src\main.cpp src\hid_win.cpp src\settings.cpp src\icons.cpp ^
    src\providers\logitech.cpp src\providers\gamesir.cpp src\providers\bluetooth.cpp ^
    build\app.res.o -o build\BatteryDeck.exe ^
    -luser32 -lshell32 -lgdi32 -lgdiplus -ladvapi32 -lsetupapi -lhid -lole32 -lshlwapi -luuid -lcfgmgr32 || exit /b 1

echo.
echo Built %CD%\build\BatteryDeck.exe
