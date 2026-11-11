@echo off
setlocal
cd /d "%~dp0"
if defined QT_ROOT set "PATH=%QT_ROOT%\bin;%PATH%"
if defined CURL_ROOT set "PATH=%CURL_ROOT%\bin;%PATH%"
if not exist "bin\AssistedRegistration.exe" (echo Build the application with build.cmd first. & exit /b 1)
start "" "%~dp0bin\AssistedRegistration.exe" --project-root "%~dp0." %*
