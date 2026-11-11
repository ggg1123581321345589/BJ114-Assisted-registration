@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0"
set "BUILD_NO_PAUSE="
if /i "%~1"=="--no-pause" set "BUILD_NO_PAUSE=1"
set "BUILD_LOG=%~dp0build.log"
set "BUILD_EXE=%~dp0bin\AssistedRegistration.exe"
echo Compiling the application. Please wait...
call :build > "%BUILD_LOG%" 2>&1
set "BUILD_RC=%errorlevel%"
type "%BUILD_LOG%"
echo(
if not "%BUILD_RC%"=="0" goto :failed
if not exist "%BUILD_EXE%" (set "BUILD_RC=1" & goto :failed)
echo [SUCCESS] EXE: "%BUILD_EXE%"
for %%I in ("%BUILD_EXE%") do echo [SUCCESS] Size: %%~zI bytes
echo [LOG] "%BUILD_LOG%"
if not defined BUILD_NO_PAUSE pause
exit /b 0
:failed
echo [FAILED] Build exit code: %BUILD_RC%
echo [FAILED] No successful new build is confirmed. An old EXE may still exist.
echo [LOG] Open "%BUILD_LOG%" for the complete error.
if not defined BUILD_NO_PAUSE pause
exit /b %BUILD_RC%
:build
echo [SOURCE] %~dp0
call :prepare_msvc
if errorlevel 1 exit /b 1
where cl >nul 2>nul
if errorlevel 1 (echo MSVC cl.exe was not found. & exit /b 1)
where nmake >nul 2>nul
if errorlevel 1 (echo MSVC nmake.exe was not found. & exit /b 1)
if defined QT_ROOT set "PATH=%QT_ROOT%\bin;%PATH%"
where qmake >nul 2>nul
if errorlevel 1 (echo Set QT_ROOT to the Qt 5.15 MSVC x64 SDK prefix. & exit /b 1)
if defined CURL_ROOT if not exist "%CURL_ROOT%\include\curl\curl.h" (echo CURL_ROOT does not contain include\curl\curl.h. & exit /b 1)
qmake -v
if not exist build mkdir build
if not exist build (echo Cannot create the build directory. & exit /b 1)
pushd build
qmake ..\AssistedRegistration.pro
if errorlevel 1 (popd & exit /b 1)
nmake /NOLOGO
set "MAKE_RC=%errorlevel%"
popd
exit /b %MAKE_RC%
:prepare_msvc
if defined VCVARS64 goto :explicit_msvc
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" if defined INCLUDE if defined LIB exit /b 0
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :missing_msvc
set "VS_INSTALL="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%I"
if not defined VS_INSTALL goto :missing_msvc
if not exist "%VS_INSTALL%\VC\Auxiliary\Build\vcvars64.bat" goto :missing_msvc
call "%VS_INSTALL%\VC\Auxiliary\Build\vcvars64.bat"
exit /b
:explicit_msvc
if not exist "%VCVARS64%" (echo VCVARS64 does not point to an existing vcvars64.bat. & exit /b 1)
call "%VCVARS64%"
exit /b
:missing_msvc
echo Open an x64 Native Tools Command Prompt or set VCVARS64 to vcvars64.bat.
exit /b 1
