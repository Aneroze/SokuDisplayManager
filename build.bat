@echo off
REM Build ExclusiveFullscreen.dll (32-bit) on Windows with the MSVC toolchain.
REM Needs Visual Studio with "Desktop development with C++" (x86 tools + Windows SDK).
setlocal

REM Use cl if it's already on PATH (e.g. run from an x86 Native Tools prompt);
REM otherwise locate any Visual Studio install via vswhere and initialize x86.
where cl >nul 2>nul && goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [!] Visual Studio not found. Open an "x86 Native Tools Command Prompt for VS" and run this again.
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL ( echo [!] VS C++ tools not found. & exit /b 1 )
call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x86 || exit /b 1

:build
set ROOT=%~dp0
set OUT=%ROOT%build
if not exist "%OUT%" mkdir "%OUT%"

cl /nologo /LD /MT /EHsc /std:c++17 /O2 ^
  /D_CRT_SECURE_NO_WARNINGS /DWINVER=0x0601 /D_WIN32_WINNT=0x0601 ^
  /I "%ROOT%src" ^
  /Fo"%OUT%\\" /Fe"%OUT%\ExclusiveFullscreen.dll" ^
  "%ROOT%src\ExclusiveFullscreen.cpp" ^
  /link shlwapi.lib user32.lib
exit /b %errorlevel%
