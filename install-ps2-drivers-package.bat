@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

rem Pin Tech Writes Code's PS2Build-native ps2_drivers revision.
set "PS2_DRIVERS_REV=54f0895756890eb8d5af25e1de954357f94bb2b6"
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

if not exist "%WORLD_ROOT%" mkdir "%WORLD_ROOT%"
if errorlevel 1 (
    echo ERROR: Could not create "%WORLD_ROOT%".
    exit /b 1
)

mkdir "%TMPROOT%" >nul 2>nul
mkdir "%SOURCE_EXPAND%" >nul 2>nul

echo Downloading pinned ps2_drivers source...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop'; Invoke-WebRequest -UseBasicParsing '%PS2_DRIVERS_SOURCE_URL%' -OutFile '%SOURCE_ZIP%'; Expand-Archive -LiteralPath '%SOURCE_ZIP%' -DestinationPath '%SOURCE_EXPAND%' -Force"
if errorlevel 1 (
    echo ERROR: Failed to download or extract ps2_drivers.
    goto :fail
)

rem Prefer the native PS2Build project shipped by Tech's fork.
set "PS2YAML="
for /f "delims=" %%P in ('dir /s /b "%SOURCE_EXPAND%\ps2.yaml" 2^>nul') do (
    if not defined PS2YAML set "PS2YAML=%%P"
)

if defined PS2YAML (
    for %%P in ("!PS2YAML!") do set "SRCDIR=%%~dpP"
    if "!SRCDIR:~-1!"=="\" set "SRCDIR=!SRCDIR:~0,-1!"

    echo.
    echo Found ps2_drivers source:
    echo   !SRCDIR!
    echo.
    echo Found native PS2Build config:
    echo   !PS2YAML!
    echo.
    echo Installing ps2_drivers with PS2Build...

    if exist "%DEST%" (
        echo Removing incomplete/old ps2_drivers package...
        rmdir /s /q "%DEST%"
    )

    pushd "!SRCDIR!" >nul
    ps2build install -c ps2.yaml --packages "%PACKAGES_ROOT%"
    set "INSTALL_RESULT=!ERRORLEVEL!"
    popd >nul

    if not "!INSTALL_RESULT!"=="0" (
        echo.
        echo ERROR: native ps2build install failed with exit code !INSTALL_RESULT!.
        goto :fail_keep
    )

    goto :finalize
)

rem Older upstream layouts do not have ps2.yaml. Keep a CMake fallback, but do
rem not use it when the native PS2Build project is available.
echo.
echo No native ps2.yaml found; falling back to upstream CMake build.

where cmake >nul 2>nul
if errorlevel 1 (
    echo ERROR: CMake is required for the legacy fallback.
    goto :fail_keep
)
where ninja >nul 2>nul
if errorlevel 1 (
    echo ERROR: Ninja is required for the legacy fallback.
    goto :fail_keep
)
if not exist "%TOOLCHAIN%" (
    echo ERROR: PS2Build CMake toolchain was not found:
    echo   "%TOOLCHAIN%"
    goto :fail_keep
)

set "CMAKELISTS="
for /f "delims=" %%P in ('dir /s /b "%SOURCE_EXPAND%\CMakeLists.txt" 2^>nul') do (
    if not defined CMAKELISTS set "CMAKELISTS=%%P"
)
if not defined CMAKELISTS (
    echo ERROR: Source contains neither ps2.yaml nor CMakeLists.txt.
    goto :fail_keep
)
for %%P in ("!CMAKELISTS!") do set "SRCDIR=%%~dpP"
if "!SRCDIR:~-1!"=="\" set "SRCDIR=!SRCDIR:~0,-1!"

echo.
echo Found ps2_drivers source:
echo   !SRCDIR!

if exist "%DEST%" rmdir /s /q "%DEST%"
mkdir "%DEST%\lib" >nul 2>nul
mkdir "%DEST%\include" >nul 2>nul
mkdir "%DEST%\lib\pkgconfig" >nul 2>nul
mkdir "%BUILDDIR%" >nul 2>nul

echo.
echo Configuring legacy CMake fallback...
cmake -S "!SRCDIR!" -B "%BUILDDIR%" -G Ninja ^
  -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN%" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DBUILD_SAMPLES=OFF ^
  -DCMAKE_INSTALL_PREFIX="%DEST%" ^
  -DINSTALL_LIB_DIR="%DEST%/lib" ^
  -DINSTALL_INC_DIR="%DEST%/include" ^
  -DINSTALL_PKGCONFIG_DIR="%DEST%/lib/pkgconfig"
if errorlevel 1 (
    echo ERROR: CMake configure failed.
    goto :fail_keep
)

echo.
echo Building ps2_drivers fallback...
cmake --build "%BUILDDIR%" --target ps2_drivers --verbose
if errorlevel 1 (
    echo ERROR: ps2_drivers CMake build failed.
    goto :fail_keep
)

echo.
echo Installing CMake fallback...
cmake --install "%BUILDDIR%"
if errorlevel 1 (
    echo ERROR: ps2_drivers CMake install failed.
    goto :fail_keep
)

:finalize
echo.
echo Finalizing PS2Build package...

rem Some revisions build correctly but omit install rules for public headers.
if not exist "%DEST%\include\ps2_filesystem_driver.h" (
    if exist "!SRCDIR!\include\ps2_filesystem_driver.h" (
        echo Copying public headers from source...
        if not exist "%DEST%\include" mkdir "%DEST%\include"
        xcopy /E /I /Y "!SRCDIR!\include\*" "%DEST%\include\" >nul
        if errorlevel 1 (
            echo ERROR: Failed to copy ps2_drivers public headers.
            goto :fail_keep
        )
    )
)

rem If the native installer placed the archive in a generated/build location,
rem normalize it into the custom package's lib directory.
if not exist "%DEST%\lib\libps2_drivers.a" (
    set "BUILT_LIB="

    for /r "%TMPROOT%" %%P in (libps2_drivers.a) do (
        if not defined BUILT_LIB set "BUILT_LIB=%%~fP"
    )

    if not defined BUILT_LIB (
        for /r "%PACKAGES_ROOT%" %%P in (libps2_drivers.a) do (
            if /i not "%%~fP"=="%DEST%\lib\libps2_drivers.a" (
                if not defined BUILT_LIB set "BUILT_LIB=%%~fP"
            )
        )
    )

    if defined BUILT_LIB (
        echo Normalizing built archive:
        echo   !BUILT_LIB!
        if not exist "%DEST%\lib" mkdir "%DEST%\lib"
        copy /Y "!BUILT_LIB!" "%DEST%\lib\libps2_drivers.a" >nul
        if errorlevel 1 (
            echo ERROR: Failed to copy libps2_drivers.a.
            goto :fail_keep
        )
    )
)

rem Keep package metadata explicit if the upstream installer did not provide it.
if not exist "%DEST%\package.yaml" (
    echo Writing compatible PS2Build package metadata...
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
)

> "%DEST%\RS2_PS2_DRIVERS.txt" echo Upstream ps2_drivers revision %PS2_DRIVERS_REV%

echo.
echo Verifying installed package...
if not exist "%DEST%\package.yaml" (
    echo ERROR: Missing "%DEST%\package.yaml".
    goto :fail_keep
)
if not exist "%DEST%\include\ps2_filesystem_driver.h" (
    echo ERROR: Missing "%DEST%\include\ps2_filesystem_driver.h".
    goto :fail_keep
)
if not exist "%DEST%\include\ps2_mouse_driver.h" (
    echo ERROR: Missing "%DEST%\include\ps2_mouse_driver.h".
    goto :fail_keep
)
if not exist "%DEST%\include\ps2_keyboard_driver.h" (
    echo ERROR: Missing "%DEST%\include\ps2_keyboard_driver.h".
    goto :fail_keep
)
if not exist "%DEST%\include\ps2_usbd_driver.h" (
    echo ERROR: Missing "%DEST%\include\ps2_usbd_driver.h".
    goto :fail_keep
)
if not exist "%DEST%\include\ps2_fileXio_driver.h" (
    echo ERROR: Missing "%DEST%\include\ps2_fileXio_driver.h".
    goto :fail_keep
)
if not exist "%DEST%\lib\libps2_drivers.a" (
    echo ERROR: Missing "%DEST%\lib\libps2_drivers.a".
    echo.
    echo Diagnostic: archives produced under the temporary source/build tree:
    for /r "%TMPROOT%" %%P in (*.a) do echo   %%~fP
    echo.
    echo Temporary files were kept for inspection:
    echo   %TMPROOT%
    goto :fail_keep
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
echo   include\ps2_usbd_driver.h
echo   include\ps2_fileXio_driver.h
echo   lib\libps2_drivers.a
echo.
echo You can now run:
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

:fail_keep
echo.
echo ERROR: ps2_drivers installation failed.
echo Temporary source/build files were kept for diagnostics:
echo   %TMPROOT%
exit /b 1
