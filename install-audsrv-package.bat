@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "AUDSRV_VERSION=v2026.09.08.1"
set "AUDSRV_URL=https://git.techwritescode.dev/ps2/audsrv/releases/download/%AUDSRV_VERSION%/audsrv-%AUDSRV_VERSION%.zip"

if "%~1"=="" (
    set "SDK_ROOT=%LOCALAPPDATA%\ps2build"
) else (
    set "SDK_ROOT=%~1"
)

set "PACKAGE_ROOT=%SDK_ROOT%\packages\world"
set "DEST=%PACKAGE_ROOT%\audsrv"
set "TMPROOT=%TEMP%\2004sp-audsrv-%RANDOM%%RANDOM%"
set "ZIP=%TMPROOT%\audsrv.zip"
set "EXPAND=%TMPROOT%\expanded"

echo ============================================================
echo  2004sp PS2 - install standalone audsrv package
echo ============================================================
echo SDK root: "%SDK_ROOT%"
echo Package : audsrv %AUDSRV_VERSION%
echo.

where powershell >nul 2>nul
if errorlevel 1 (
    echo ERROR: PowerShell is required to download/extract the package.
    exit /b 1
)

if not exist "%PACKAGE_ROOT%" (
    echo ERROR: "%PACKAGE_ROOT%" does not exist.
    echo.
    echo Current PS2Build installs default to:
    echo   %%LOCALAPPDATA%%\ps2build
    echo.
    echo If your SDK is elsewhere, pass its root as the first argument:
    echo   install-audsrv-package.bat D:\path\to\ps2build
    exit /b 1
)

mkdir "%TMPROOT%" >nul 2>nul
mkdir "%EXPAND%" >nul 2>nul

echo Downloading pinned audsrv release...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop'; Invoke-WebRequest -UseBasicParsing '%AUDSRV_URL%' -OutFile '%ZIP%'; Expand-Archive -LiteralPath '%ZIP%' -DestinationPath '%EXPAND%' -Force"
if errorlevel 1 goto :fail

for /f "delims=" %%P in ('powershell -NoProfile -Command "$p=Get-ChildItem -Path '%EXPAND%' -Filter package.yaml -Recurse ^| Select-Object -First 1; if($p){$p.Directory.FullName}"') do set "PKGSRC=%%P"
if not defined PKGSRC (
    echo ERROR: Download did not contain package.yaml.
    goto :fail
)

if not exist "!PKGSRC!\lib\libaudsrv.a" (
    echo ERROR: Package is missing lib\libaudsrv.a.
    goto :fail
)
if not exist "!PKGSRC!\bin\audsrv.irx" (
    echo ERROR: Package is missing bin\audsrv.irx.
    goto :fail
)
if not exist "!PKGSRC!\include\audsrv.h" (
    echo ERROR: Package is missing include\audsrv.h.
    goto :fail
)

if exist "%DEST%" (
    echo Replacing existing "%DEST%"...
    rmdir /s /q "%DEST%"
)
mkdir "%DEST%" >nul 2>nul
xcopy /e /i /y "!PKGSRC!\*" "%DEST%\" >nul
if errorlevel 1 goto :fail

echo.
echo Installed:
echo   %DEST%
echo.
echo Verified:
echo   package.yaml
echo   include\audsrv.h
echo   lib\libaudsrv.a
echo   bin\audsrv.irx
echo.
echo You can now run:
echo   ps2build build
echo.
rmdir /s /q "%TMPROOT%" >nul 2>nul
exit /b 0

:fail
echo.
echo ERROR: audsrv installation failed.
if exist "%TMPROOT%" rmdir /s /q "%TMPROOT%" >nul 2>nul
exit /b 1
