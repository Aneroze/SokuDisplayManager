@echo off
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -property installationPath`) do set VSPATH=%%i
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cl /nologo /LD /O2 /EHsc /DNDEBUG SamplerProbe.cpp /link /OUT:SamplerProbe.dll shlwapi.lib user32.lib gdi32.lib
