@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0"

 echo.
 echo ===========================================================
 echo  PLANE RADAR v2.0.1 - UPLOAD TO ESP32-2424S012
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
if errorlevel 1 goto uploadFailed

echo.
echo Building from scratch and uploading environment supermini...
echo Verify your board is connected via USB.
echo.
"%PIO_PY%" -m platformio run -e supermini -t upload
if errorlevel 1 goto uploadFailed

echo.
echo v2.0.1 CLEAN BUILD AND UPLOAD SUCCESSFUL.
echo.
pause
exit /b 0

:findPython
rem Reuses the detection method from BUILD_WITH_PLATFORMIO.bat.
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

:uploadFailed
echo.
echo ERROR: Build/upload failed; check the output above.
echo Nothing was reported as successfully flashed.
echo.
pause
exit /b 1

:missingPlatformIO
echo.
echo ERROR: Could not locate an initialized PlatformIO Core Python.
echo Open VS Code ^> PlatformIO Home and let Core initialization finish.
echo.
pause
exit /b 1
