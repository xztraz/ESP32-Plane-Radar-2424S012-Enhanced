@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0"

 echo.
 echo ===========================================================
 echo  PLANE RADAR v2.0.0 - ESP32-2424S012 NON-TOUCH
 echo  PlatformIO Core Python - no Git or separate Python needed
 echo ===========================================================
 echo.

rem The release includes data\ui_font.vlw. Never borrow a font from an old build.
if not exist "data\ui_font.vlw" (
  echo ERROR: Required data\ui_font.vlw is missing from this package.
  pause
  exit /b 1
)

set "PIO_PY="
call :findPython
if not defined PIO_PY goto missingPlatformIO

echo Found PlatformIO Core Python:
echo "%PIO_PY%"
echo.
echo CLEAN BUILD: removing old compiled objects for supermini...
echo Existing PlatformIO downloads and library packages are retained.
echo.
"%PIO_PY%" -m platformio run -e supermini -t clean
if errorlevel 1 goto buildFailed

rem Avoid accidentally using a stale merged image if merging later fails.
if exist "%~dp0.pio\build\supermini\firmware-merged.bin" del /f /q "%~dp0.pio\build\supermini\firmware-merged.bin"
if exist "%~dp0.pio\build\supermini\firmware-merged.bin" (
  echo ERROR: Could not remove the old merged firmware image.
  goto buildFailed
)

echo.
echo Building PlatformIO environment supermini from scratch...
echo.
"%PIO_PY%" -m platformio run -e supermini
if errorlevel 1 goto buildFailed

echo.
echo Building single merged flash image...
echo.
"%PIO_PY%" -m platformio run -e supermini -t merge
if errorlevel 1 goto buildFailed

if not exist "%~dp0.pio\build\supermini\firmware-merged.bin" (
  echo.
  echo ERROR: PlatformIO finished but firmware-merged.bin is missing.
  echo DO NOT flash an old or incomplete file.
  echo.
  pause
  exit /b 1
)

echo.
echo ===========================================================
echo  v2.0.0 CLEAN BUILD SUCCESSFUL
echo ===========================================================
echo.
echo Normal PlatformIO firmware:
echo "%~dp0.pio\build\supermini\firmware.bin"
echo.
echo Single flashable image ^(offset 0x0000^):
echo "%~dp0.pio\build\supermini\firmware-merged.bin"
echo.
pause
exit /b 0

:findPython
rem Reuses the detection method from BUILD_WITH_PLATFORMIO.bat.
rem Verify that the candidate Python can import PlatformIO, not just that a file exists.
if defined PIO_PYTHON call :tryPython "%PIO_PYTHON%"
if defined PLATFORMIO_CORE_DIR call :tryPython "%PLATFORMIO_CORE_DIR%\penv\Scripts\python.exe"
if defined VIRTUAL_ENV call :tryPython "%VIRTUAL_ENV%\Scripts\python.exe"
call :tryPython "%USERPROFILE%\.platformio\penv\Scripts\python.exe"
call :tryPython "%HOMEDRIVE%%HOMEPATH%\.platformio\penv\Scripts\python.exe"
call :tryPython "%SystemDrive%\.platformio\penv\Scripts\python.exe"
call :tryPython "C:\.platformio\penv\Scripts\python.exe"
call :tryPython "%LOCALAPPDATA%\platformio\penv\Scripts\python.exe"
call :tryPython "%APPDATA%\Code\User\globalStorage\platformio.platformio-ide\penv\Scripts\python.exe"
for /f "delims=" %%F in ('where pio.exe 2^>nul') do call :tryPython "%%~dpFpython.exe"
for /f "delims=" %%F in ('where platformio.exe 2^>nul') do call :tryPython "%%~dpFpython.exe"
for /f "delims=" %%F in ('where python.exe 2^>nul') do call :tryPython "%%F"
exit /b 0

:tryPython
if defined PIO_PY exit /b 0
if not exist "%~1" exit /b 0
"%~1" -c "import platformio" >nul 2>nul
if errorlevel 1 exit /b 0
set "PIO_PY=%~1"
exit /b 0

:buildFailed
echo.
echo ERROR: v2.0.0 build or merge failed. DO NOT flash incomplete output.
echo Copy the error text from this window.
echo.
pause
exit /b 1

:missingPlatformIO
echo.
echo ERROR: Could not locate an initialized PlatformIO Core Python.
echo This script checks the same locations as your working old build script.
echo.
echo Open VS Code ^> PlatformIO Home and let Core initialization finish.
echo If the old script works but this one fails, check whether the old
 echo build was run from a different Windows user account or PC.
echo.
pause
exit /b 1
