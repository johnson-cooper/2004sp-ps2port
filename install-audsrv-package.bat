@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "AUDSRV_VERSION=v2026.09.08.1"
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

if not exist "%PACKAGES_ROOT%" (
    echo ERROR: "%PACKAGES_ROOT%" does not exist.
    echo.
    echo Current PS2Build installs default to:
    echo   %%LOCALAPPDATA%%\ps2build
    echo.
    echo If your SDK is elsewhere, pass its root as the first argument:
    echo   install-audsrv-package.bat D:\path\to\ps2build
    exit /b 1
)

if not exist "%WORLD_ROOT%" mkdir "%WORLD_ROOT%"
if errorlevel 1 (
    echo ERROR: Could not create "%WORLD_ROOT%".
    exit /b 1
)

mkdir "%TMPROOT%" >nul 2>nul
mkdir "%SOURCE_EXPAND%" >nul 2>nul

echo Downloading pinned audsrv source...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop'; Invoke-WebRequest -UseBasicParsing '%AUDSRV_SOURCE_URL%' -OutFile '%SOURCE_ZIP%'; Expand-Archive -LiteralPath '%SOURCE_ZIP%' -DestinationPath '%SOURCE_EXPAND%' -Force"
if errorlevel 1 (
    echo ERROR: Failed to download or extract audsrv source.
    goto :fail
)

set "PS2YAML="
for /f "delims=" %%P in ('dir /s /b "%SOURCE_EXPAND%\ps2.yaml" 2^>nul') do (
    if not defined PS2YAML set "PS2YAML=%%P"
)

if not defined PS2YAML (
    echo ERROR: Extracted audsrv source does not contain ps2.yaml.
    echo Extracted files were left temporarily at:
    echo   %SOURCE_EXPAND%
    goto :fail
)

for %%P in ("!PS2YAML!") do set "SRCDIR=%%~dpP"

if not exist "!SRCDIR!package.yaml" (
    echo ERROR: Found ps2.yaml but no package.yaml beside it:
    echo   !SRCDIR!
    goto :fail
)

echo.
echo Found audsrv source:
echo   !SRCDIR!
echo.
echo Applying the 2004sp ADPCM-only audsrv patch...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\ps2_audio\patch_audsrv_voice_only.ps1" -SourceDir "!SRCDIR!"
if errorlevel 1 (
    echo ERROR: Failed to patch audsrv for ADPCM-only operation.
    goto :fail
)
echo.
echo Building and installing custom audsrv with PS2Build...
echo   packages: "%PACKAGES_ROOT%"
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
    echo.
    echo ERROR: ps2build install failed with exit code !INSTALL_RESULT!.
    goto :fail
)

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
> "%DEST%\RS2_VOICE_ONLY.txt" echo 2004sp audsrv voice-only patch - no PCM streaming thread or looping block DMA
echo   RS2_VOICE_ONLY.txt
echo.
echo You can now run:
echo   ps2build build
echo.

if exist "%TMPROOT%" rmdir /s /q "%TMPROOT%" >nul 2>nul
exit /b 0

:fail
echo.
echo ERROR: audsrv installation failed.
echo The accepted RuneScape hardware baseline is unchanged.
if exist "%TMPROOT%" rmdir /s /q "%TMPROOT%" >nul 2>nul
exit /b 1
