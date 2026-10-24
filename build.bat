@echo off
rem Builds build\BatteryDeck.exe with MSVC (Visual Studio or Build Tools, "Desktop development with C++").
rem Use build-mingw.bat for LLVM-MinGW instead.
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe not found - is Visual Studio / Build Tools installed?
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo No MSVC x64 toolset found. Install the "Desktop development with C++" workload.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist build mkdir build

rc /nologo /fo build\app.res res\app.rc || exit /b 1

rem /O1 /GL: small, fast code. /MT: static CRT, so no VC++ redistributable is needed.
cl /nologo /std:c++20 /O1 /GL /Gy /MT /EHsc /W4 /permissive- /utf-8 ^
   /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 ^
   /Fobuild\ /Fdbuild\ /Febuild\BatteryDeck.exe ^
   src\main.cpp src\hid_win.cpp src\settings.cpp src\icons.cpp ^
   src\providers\logitech.cpp src\providers\gamesir.cpp src\providers\bluetooth.cpp build\app.res ^
   /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS /MANIFEST:NO ^
   user32.lib shell32.lib gdi32.lib gdiplus.lib advapi32.lib setupapi.lib hid.lib ole32.lib shlwapi.lib uuid.lib cfgmgr32.lib || exit /b 1

echo.
echo Built %CD%\build\BatteryDeck.exe
