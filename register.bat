@echo off
rem ===========================================================================
rem  Додає PicoView у список "Відкрити за допомогою" для зображень.
rem
rem  Пише ЛИШЕ в HKEY_CURRENT_USER, прав адміністратора не потребує і НЕ робить
rem  програму стандартною — Windows 11 цього не дозволяє програмам. Після
rem  запуску скрипта оберіть PicoView у Провіднику через "Відкрити за
rem  допомогою", або в Параметри -> Програми -> Стандартні програми.
rem
rem    register.bat           додати
rem    register.bat /remove   прибрати
rem ===========================================================================
setlocal enabledelayedexpansion
set "EXE=%~dp0PicoView.exe"
set "APPKEY=HKCU\Software\Classes\Applications\PicoView.exe"
set "TYPES=.jpg .jpeg .jfif .png .gif .bmp .tif .tiff .ico .webp .heic .heif .avif .dds .tga .psd .jxr .wdp .hdr .qoi .ppm .pgm .pbm .pnm"

if /i "%~1"=="/remove" goto :remove
if /i "%~1"=="-remove" goto :remove
if /i "%~1"=="/u" goto :remove

if not exist "%EXE%" (
  echo [!] PicoView.exe не знайдено поруч зі скриптом. Спочатку запустіть build.bat
  exit /b 1
)

echo Реєструю PicoView для поточного користувача...
reg add "%APPKEY%" /v FriendlyAppName /t REG_SZ /d "PicoView" /f >nul
reg add "%APPKEY%\shell\open\command" /ve /t REG_SZ /d "\"%EXE%\" \"%%1\"" /f >nul
reg add "%APPKEY%\DefaultIcon" /ve /t REG_SZ /d "\"%EXE%\",0" /f >nul

for %%T in (%TYPES%) do (
  reg add "%APPKEY%\SupportedTypes" /v %%T /t REG_SZ /d "" /f >nul
  reg add "HKCU\Software\Classes\%%T\OpenWithList\PicoView.exe" /f >nul
)

echo.
echo   Готово. PicoView тепер є в меню "Відкрити за допомогою".
echo   Щоб зробити стандартною: Параметри -^> Програми -^> Стандартні програми
echo   -^> PicoView.
goto :eof

:remove
echo Прибираю реєстрацію...
reg delete "%APPKEY%" /f >nul 2>&1
for %%T in (%TYPES%) do reg delete "HKCU\Software\Classes\%%T\OpenWithList\PicoView.exe" /f >nul 2>&1
echo   Готово.
