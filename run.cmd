@echo off
setlocal
cd /d "%~dp0"
if not exist "%~dp0bin\AssistedRegistration.exe" (
  echo Build first: build.cmd
  pause
  exit /b 1
)
start "" "%~dp0bin\AssistedRegistration.exe" --project-root "%~dp0."
