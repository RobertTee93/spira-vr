@echo off
setlocal
cd /d "%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
 echo Install Visual Studio 2022 Build Tools with Desktop development with C++.
 exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL exit /b 1
call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 exit /b 1
if not exist build mkdir build
pushd build
cl /nologo /LD /MT /O2 /EHsc /std:c++17 /I..\third_party\reshade /I..\third_party\openvr /I..\third_party\minhook\include ..\src\headlook.cpp ..\third_party\minhook\src\buffer.c ..\third_party\minhook\src\hook.c ..\third_party\minhook\src\trampoline.c ..\third_party\minhook\src\hde\hde32.c /link /OUT:..\ffx-headlook.addon32 user32.lib
set "BUILD_RESULT=%ERRORLEVEL%"
popd
exit /b %BUILD_RESULT%
