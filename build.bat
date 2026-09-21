@echo off
rem ===========================================================================
rem  PicoView - build script
rem  Produces a single portable PicoView.exe (static CRT, no dependencies).
rem  Requires Visual Studio Build Tools with the "Desktop development with C++"
rem  workload.  Just run:  build.bat
rem ===========================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

if "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :have_env

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [!] Visual Studio Installer not found.
  echo     Install "Build Tools for Visual Studio" with the C++ workload.
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
  echo [!] vcvars64.bat not found - the C++ toolset is not installed.
  exit /b 1
)
call "%VCVARS%" >nul || (echo [!] vcvars64 failed & exit /b 1)

:have_env
if not exist build mkdir build

echo [1/3] resources
rc /nologo /fo build\app.res res\app.rc || exit /b 1

echo [2/3] shaders
rem  Compiled at build time, not at run time: D3DCompile would drag in
rem  d3dcompiler_47.dll and the point of this program is one file.
fxc /nologo /T vs_4_0 /E VSMain /O3 /Fh build\blob_vs.h /Vn kBlobVS src\blob.hlsl || exit /b 1
fxc /nologo /T ps_4_0 /E PSMain /O3 /Fh build\blob_ps.h /Vn kBlobPS src\blob.hlsl || exit /b 1

echo [3/3] compiling
set CFLAGS=/nologo /std:c++17 /utf-8 /permissive- /Zc:__cplusplus /W3 /MP /EHsc /DUNICODE /D_UNICODE /O2 /Oi /Gy /DNDEBUG /MT /I build
rem  mf.dll is only touched by the converter's worker thread, and winhttp and
rem  bcrypt only when someone opens the plugin catalogue, so all three are
rem  delay loaded: nothing about opening a picture should wait for them.
set LFLAGS=/link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /INCREMENTAL:NO /DELAYLOAD:mf.dll ^
 /DELAYLOAD:winhttp.dll /DELAYLOAD:bcrypt.dll
set LIBS=user32.lib gdi32.lib shell32.lib shlwapi.lib ole32.lib oleaut32.lib uuid.lib ^
 comdlg32.lib advapi32.lib propsys.lib d3d11.lib dxgi.lib d2d1.lib dwrite.lib dcomp.lib ^
 windowscodecs.lib dwmapi.lib mfplat.lib mfuuid.lib mfreadwrite.lib mf.lib delayimp.lib ^
 winhttp.lib bcrypt.lib

cl %CFLAGS% /Fo:build\ /Fd:build\ src\main.cpp src\ui.cpp src\gfx.cpp src\decode.cpp ^
   src\loader.cpp src\util.cpp src\video.cpp src\lang.cpp src\encode.cpp ^
   src\convert.cpp src\envelope.cpp src\loudness.cpp src\coverart.cpp src\blob.cpp ^
   src\plugins.cpp src\store.cpp ^
   src\core\coverplan.cpp src\core\loudness_math.cpp ^
   build\app.res /Fe:PicoView.exe %LFLAGS% %LIBS% || exit /b 1

echo.
for %%F in (PicoView.exe) do echo    PicoView.exe  -  %%~zF bytes
echo    Done.
endlocal
