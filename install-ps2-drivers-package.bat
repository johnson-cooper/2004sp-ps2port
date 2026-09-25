@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

rem Pin the exact upstream revision reviewed for the MMCE/filesystem/HID integration.
set "PS2_DRIVERS_REV=3163ae180eb2117d1ebe67bfa01fdda7a76d35ad"
set "PS2_DRIVERS_SOURCE_URL=https://git.techwritescode.dev/ps2/ps2_drivers/archive/%PS2_DRIVERS_REV%.zip"

if not "%~1"=="" (
    set "SDK_ROOT=%~1"
) else if defined PS2DEV (
    set "SDK_ROOT=%PS2DEV%"
) else (
    set "SDK_ROOT=%LOCALAPPDATA%\ps2build"
)

set "PACKAGES_ROOT=%SDK_ROOT%\packages"
set "WORLD_ROOT=%PACKAGES_ROOT%\world"
set "DEST=%WORLD_ROOT%\ps2_drivers"
set "TOOLCHAIN=%SDK_ROOT%\cmake\ps2dev.cmake"
set "TMPROOT=%TEMP%\2004sp-ps2-drivers-%RANDOM%%RANDOM%"
set "SOURCE_ZIP=%TMPROOT%\ps2_drivers-source.zip"
set "SOURCE_EXPAND=%TMPROOT%\source"
set "BUILDDIR=%TMPROOT%\build"

echo ============================================================
echo  2004sp PS2 - install standalone ps2_drivers package
echo ============================================================
echo SDK root: "%SDK_ROOT%"
if defined PS2DEV echo PS2DEV : "%PS2DEV%"
echo Revision: %PS2_DRIVERS_REV%
echo Package : "%DEST%"
echo.

where powershell >nul 2>nul
if errorlevel 1 (
    echo ERROR: PowerShell is required to download/extract ps2_drivers.
    exit /b 1
)

where cmake >nul 2>nul
if errorlevel 1 (
    echo ERROR: CMake is required to build the standalone ps2_drivers package.
    exit /b 1
)

where ninja >nul 2>nul
if errorlevel 1 (
    echo ERROR: Ninja is required to build ps2_drivers.
    echo Start the PS2Build environment first, then run this script again.
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
    echo Pass the PS2Build SDK root explicitly if needed:
    echo   install-ps2-drivers-package.bat D:\path\to\ps2build
    exit /b 1
)

if not exist "%TOOLCHAIN%" (
    echo ERROR: PS2Build CMake toolchain was not found:
    echo   "%TOOLCHAIN%"
    echo Run "ps2build update" first, then retry.
    exit /b 1
)

if not exist "%WORLD_ROOT%" mkdir "%WORLD_ROOT%"
if errorlevel 1 (
    echo ERROR: Could not create "%WORLD_ROOT%".
    exit /b 1
)

mkdir "%TMPROOT%" >nul 2>nul
mkdir "%SOURCE_EXPAND%" >nul 2>nul
mkdir "%BUILDDIR%" >nul 2>nul

echo Downloading pinned ps2_drivers source...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop'; Invoke-WebRequest -UseBasicParsing '%PS2_DRIVERS_SOURCE_URL%' -OutFile '%SOURCE_ZIP%'; Expand-Archive -LiteralPath '%SOURCE_ZIP%' -DestinationPath '%SOURCE_EXPAND%' -Force"
if errorlevel 1 (
    echo ERROR: Failed to download or extract ps2_drivers.
    goto :fail
)

set "CMAKELISTS="
for /f "delims=" %%P in ('dir /s /b "%SOURCE_EXPAND%\CMakeLists.txt" 2^>nul') do (
    if not defined CMAKELISTS set "CMAKELISTS=%%P"
)
if not defined CMAKELISTS (
    echo ERROR: Extracted ps2_drivers source does not contain CMakeLists.txt.
    goto :fail
)
for %%P in ("!CMAKELISTS!") do set "SRCDIR=%%~dpP"
if "!SRCDIR:~-1!"=="\" set "SRCDIR=!SRCDIR:~0,-1!"

echo.
echo Found ps2_drivers source:
echo   !SRCDIR!
echo.
echo Configuring with the PS2Build CMake toolchain...

if exist "%DEST%" (
    echo Removing incomplete/old ps2_drivers package...
    rmdir /s /q "%DEST%"
)
mkdir "%DEST%\lib" >nul 2>nul
mkdir "%DEST%\include" >nul 2>nul
mkdir "%DEST%\lib\pkgconfig" >nul 2>nul

cmake -S "!SRCDIR!" -B "%BUILDDIR%" -G Ninja ^
  -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN%" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DBUILD_SAMPLES=OFF ^
  -DINSTALL_LIB_DIR="%DEST%\lib" ^
  -DINSTALL_INC_DIR="%DEST%\include" ^
  -DINSTALL_PKGCONFIG_DIR="%DEST%\lib\pkgconfig"
if errorlevel 1 (
    echo ERROR: CMake configure failed.
    goto :fail
)

echo.
echo Building ps2_drivers...
cmake --build "%BUILDDIR%" --target ps2_drivers
if errorlevel 1 (
    echo ERROR: ps2_drivers build failed.
    goto :fail
)

echo.
echo Installing ps2_drivers into the PS2Build world package tree...
cmake --install "%BUILDDIR%"
if errorlevel 1 (
    echo ERROR: ps2_drivers install failed.
    goto :fail
)

echo.
echo Writing PS2Build package metadata...
> "%DEST%\package.yaml" echo name: ps2_drivers
>>"%DEST%\package.yaml" echo origin: ps2_drivers
>>"%DEST%\package.yaml" echo tier: world
>>"%DEST%\package.yaml" echo license: LGPL-2.0-only
>>"%DEST%\package.yaml" echo.
>>"%DEST%\package.yaml" echo artifacts:
>>"%DEST%\package.yaml" echo   - kind: library
>>"%DEST%\package.yaml" echo     target: [ee]
>>"%DEST%\package.yaml" echo     include_dirs: [include]
>>"%DEST%\package.yaml" echo     lib: lib/libps2_drivers.a

> "%DEST%\RS2_PS2_DRIVERS.txt" echo Upstream ps2_drivers revision %PS2_DRIVERS_REV%

echo.
echo Verifying installed package...
if not exist "%DEST%\package.yaml" (
    echo ERROR: Missing "%DEST%\package.yaml".
    goto :fail
)
if not exist "%DEST%\include\ps2_filesystem_driver.h" (
    echo ERROR: Missing "%DEST%\include\ps2_filesystem_driver.h".
    goto :fail
)
if not exist "%DEST%\include\ps2_mouse_driver.h" (
    echo ERROR: Missing "%DEST%\include\ps2_mouse_driver.h".
    goto :fail
)
if not exist "%DEST%\include\ps2_keyboard_driver.h" (
    echo ERROR: Missing "%DEST%\include\ps2_keyboard_driver.h".
    goto :fail
)
if not exist "%DEST%\lib\libps2_drivers.a" (
    echo ERROR: Missing "%DEST%\lib\libps2_drivers.a".
    goto :fail
)

echo.
echo Installed successfully:
echo   %DEST%
echo.
echo Verified:
echo   package.yaml
echo   include\ps2_filesystem_driver.h
echo   include\ps2_mouse_driver.h
echo   include\ps2_keyboard_driver.h
echo   lib\libps2_drivers.a
echo.
echo PS2Build can now resolve:
echo   libs: [ps2_drivers]
echo.
echo Now run your normal:
echo   ps2build build
echo.

if exist "%TMPROOT%" rmdir /s /q "%TMPROOT%" >nul 2>nul
exit /b 0

:fail
echo.
echo ERROR: ps2_drivers installation failed.
if exist "%DEST%" rmdir /s /q "%DEST%" >nul 2>nul
if exist "%TMPROOT%" rmdir /s /q "%TMPROOT%" >nul 2>nul
exit /b 1
