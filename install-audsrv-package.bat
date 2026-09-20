@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "AUDSRV_VERSION=v2026.09.08.1"
set "AUDSRV_RELEASE_URL=https://git.techwritescode.dev/ps2/audsrv/releases/download/%AUDSRV_VERSION%/audsrv-%AUDSRV_VERSION%.zip"
set "AUDSRV_SOURCE_URL=https://git.techwritescode.dev/ps2/audsrv/archive/%AUDSRV_VERSION%.zip"

if "%~1"=="" (
    set "SDK_ROOT=%LOCALAPPDATA%\ps2build"
) else (
    set "SDK_ROOT=%~1"
)

set "PACKAGES_ROOT=%SDK_ROOT%\packages"
set "WORLD_ROOT=%PACKAGES_ROOT%\world"
set "DEST=%WORLD_ROOT%\audsrv"
set "TMPROOT=%TEMP%\2004sp-audsrv-%RANDOM%%RANDOM%"
set "RELEASE_ZIP=%TMPROOT%\audsrv-release.zip"
set "RELEASE_EXPAND=%TMPROOT%\release"
set "SOURCE_ZIP=%TMPROOT%\audsrv-source.zip"
set "SOURCE_EXPAND=%TMPROOT%\source"

echo ============================================================
echo  2004sp PS2 - install standalone audsrv package
echo ============================================================
echo SDK root: "%SDK_ROOT%"
echo Package : audsrv %AUDSRV_VERSION%
echo.

where powershell >nul 2>nul
if errorlevel 1 (
    echo ERROR: PowerShell is required to download/extract audsrv.
    exit /b 1
)

where ps2build >nul 2>nul
if errorlevel 1 (
    echo ERROR: ps2build is not available on PATH.
    echo Start the PS2Build environment first, then run this script again.
    exit /b 1
)

if not exist "%WORLD_ROOT%" (
    echo ERROR: "%WORLD_ROOT%" does not exist.
    echo.
    echo Current PS2Build installs default to:
    echo   %%LOCALAPPDATA%%\ps2build
    echo.
    echo If your SDK is elsewhere, pass its root as the first argument:
    echo   install-audsrv-package.bat D:\path\to\ps2build
    exit /b 1
)

mkdir "%TMPROOT%" >nul 2>nul
mkdir "%RELEASE_EXPAND%" >nul 2>nul
mkdir "%SOURCE_EXPAND%" >nul 2>nul

echo Downloading pinned audsrv release package...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop'; Invoke-WebRequest -UseBasicParsing '%AUDSRV_RELEASE_URL%' -OutFile '%RELEASE_ZIP%'; Expand-Archive -LiteralPath '%RELEASE_ZIP%' -DestinationPath '%RELEASE_EXPAND%' -Force"
if errorlevel 1 goto :source_fallback

set "PKGSRC="
for /f "delims=" %%P in ('powershell -NoProfile -Command "$p=Get-ChildItem -Path '%RELEASE_EXPAND%' -Filter package.yaml -Recurse ^| Where-Object { (Test-Path (Join-Path $_.Directory.FullName 'lib\libaudsrv.a')) -and (Test-Path (Join-Path $_.Directory.FullName 'bin\audsrv.irx')) -and (Test-Path (Join-Path $_.Directory.FullName 'include\audsrv.h')) } ^| Select-Object -First 1; if($p){$p.Directory.FullName}"') do set "PKGSRC=%%P"

if defined PKGSRC (
    echo Found complete prebuilt package:
    echo   "!PKGSRC!"

    if exist "%DEST%" (
        echo Replacing existing "%DEST%"...
        rmdir /s /q "%DEST%"
    )

    mkdir "%DEST%" >nul 2>nul
    xcopy /e /i /y "!PKGSRC!\*" "%DEST%\" >nul
    if errorlevel 1 goto :fail

    goto :verify
)

echo.
echo Release archive did not contain a complete installed package.
echo Falling back to the official source build/install path.
echo.

:source_fallback
echo Downloading pinned audsrv source...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop'; Invoke-WebRequest -UseBasicParsing '%AUDSRV_SOURCE_URL%' -OutFile '%SOURCE_ZIP%'; Expand-Archive -LiteralPath '%SOURCE_ZIP%' -DestinationPath '%SOURCE_EXPAND%' -Force"
if errorlevel 1 goto :fail

set "SRCDIR="
for /f "delims=" %%P in ('powershell -NoProfile -Command "$p=Get-ChildItem -Path '%SOURCE_EXPAND%' -Filter ps2.yaml -Recurse ^| Where-Object { Test-Path (Join-Path $_.Directory.FullName 'package.yaml') } ^| Select-Object -First 1; if($p){$p.Directory.FullName}"') do set "SRCDIR=%%P"

if not defined SRCDIR (
    echo ERROR: Source archive did not contain ps2.yaml + package.yaml.
    goto :fail
)

echo Building and installing audsrv with the official PS2Build path...
echo   source   : "!SRCDIR!"
echo   packages : "%PACKAGES_ROOT%"
echo.

if exist "%DEST%" (
    echo Removing incomplete/old audsrv package...
    rmdir /s /q "%DEST%"
)

pushd "!SRCDIR!" >nul
ps2build install -c ps2.yaml --packages "%PACKAGES_ROOT%"
set "INSTALL_RESULT=!ERRORLEVEL!"
popd >nul

if not "!INSTALL_RESULT!"=="0" (
    echo ERROR: ps2build install failed with exit code !INSTALL_RESULT!.
    goto :fail
)

:verify
echo.
echo Verifying installed package...

if not exist "%DEST%\package.yaml" (
    echo ERROR: Missing "%DEST%\package.yaml".
    goto :fail
)
if not exist "%DEST%\include\audsrv.h" (
    echo ERROR: Missing "%DEST%\include\audsrv.h".
    goto :fail
)
if not exist "%DEST%\lib\libaudsrv.a" (
    echo ERROR: Missing "%DEST%\lib\libaudsrv.a".
    goto :fail
)
if not exist "%DEST%\bin\audsrv.irx" (
    echo ERROR: Missing "%DEST%\bin\audsrv.irx".
    goto :fail
)

echo.
echo Installed successfully:
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

if exist "%TMPROOT%" rmdir /s /q "%TMPROOT%" >nul 2>nul
exit /b 0

:fail
echo.
echo ERROR: audsrv installation failed.
echo The current RuneScape hardware baseline has not been modified.
if exist "%TMPROOT%" rmdir /s /q "%TMPROOT%" >nul 2>nul
exit /b 1
