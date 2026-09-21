@echo off
rem ===========================================================================
rem  PicoView plugin: quick preview on hover in Explorer.
rem  Produces plugins\hoverpeek.dll
rem ===========================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

if "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :have_env

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [!] Visual Studio Installer not found.
  exit /b 1
)
set "VCVARS="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -products * -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
  if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -products * -all -property installationPath 2^>nul`) do (
    if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
  )
)
if not defined VCVARS (
  echo [!] vcvars64.bat not found.
  exit /b 1
)
call "%VCVARS%" >nul || (echo [!] vcvars64 failed & exit /b 1)

:have_env
if not exist ..\..\plugins mkdir ..\..\plugins
if not exist build mkdir build

rem  /MT so the DLL carries its own runtime, like the viewer itself.
cl /nologo /std:c++17 /EHsc /W3 /O2 /Oi /Gy /DNDEBUG /MT /DUNICODE /D_UNICODE /LD ^
   /Fo:build\ /Fd:build\ hoverpeek.cpp /Fe:..\hoverpeek.dll ^
   /link /DLL /OPT:REF /OPT:ICF ^
   user32.lib gdi32.lib ole32.lib oleaut32.lib uuid.lib shell32.lib shlwapi.lib msimg32.lib || exit /b 1

del ..\hoverpeek.exp ..\hoverpeek.lib 2>nul
for %%F in (..\hoverpeek.dll) do echo    hoverpeek.dll  -  %%~zF bytes
echo    Done.
