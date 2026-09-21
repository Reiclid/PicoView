@echo off
rem ===========================================================================
rem  Example PicoView plugin.
rem  Produces plugins\upscale.dll - drop it beside PicoView.exe in a folder
rem  called "plugins" and restart the viewer.
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

rem  /MT so the DLL carries its own runtime: a plugin must not need anything
rem  installed that the single-file viewer does not.
cl /nologo /W3 /O2 /Oi /Gy /DNDEBUG /MT /LD /Fo:build\ /Fd:build\ ^
   sample.c /Fe:..\upscale.dll /link /DLL /OPT:REF /OPT:ICF user32.lib kernel32.lib || exit /b 1

del ..\upscale.exp ..\upscale.lib 2>nul
for %%F in (..\upscale.dll) do echo    upscale.dll  -  %%~zF bytes
echo    Done.
