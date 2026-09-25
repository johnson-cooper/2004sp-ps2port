@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

rem Pin Tech Writes Code's PS2Build-native main revision: it resolves dependencies from
rem %%PS2DEV%%\packages\{core,world} instead of the obsolete flat %%PS2SDK%% tree.
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
  -DCMAKE_INSTALL_PREFIX="%DEST%" ^
  -DINSTALL_LIB_DIR="%DEST%/lib" ^
  -DINSTALL_INC_DIR="%DEST%/include" ^
  -DINSTALL_PKGCONFIG_DIR="%DEST%/lib/pkgconfig"
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
echo Built ps2_drivers archives:
set "FOUND_ARCHIVE=0"
for /r "%BUILDDIR%" %%P in (*ps2_drivers*.a) do (
    echo   %%~fP
    set "FOUND_ARCHIVE=1"
)
if "!FOUND_ARCHIVE!"=="0" (
    echo   WARNING: no *ps2_drivers*.a archive was found under the CMake build directory.
)

echo.
echo Installing ps2_drivers into the PS2Build world package tree...
cmake --install "%BUILDDIR%"
if errorlevel 1 (
    echo ERROR: ps2_drivers install failed.
    goto :fail
)

rem PS2Build's custom-package contract is packages\world\<name>. Upstream
rem ps2_drivers still installs through its INSTALL_LIB_DIR / INSTALL_INC_DIR cache
rem variables, so set those explicitly in addition to CMAKE_INSTALL_PREFIX.
rem Keep two defensive fallbacks:
rem upstream revisions have changed their CMake install details more than once,
rem but the public headers and the built archive are stable inputs we can place
rem into the package deterministically if an install rule omits them.
if not exist "%DEST%\include\ps2_filesystem_driver.h" (
    if exist "!SRCDIR!\include\ps2_filesystem_driver.h" (
        echo.
        echo CMake did not install the public headers; copying them from source...
        if not exist "%DEST%\include" mkdir "%DEST%\include"
        xcopy /E /I /Y "!SRCDIR!\include\*" "%DEST%\include\" >nul
        if errorlevel 1 (
            echo ERROR: Failed to copy ps2_drivers public headers.
            goto :fail
        )
    )
)

if not exist "%DEST%\lib\libps2_drivers.a" (
    set "BUILT_LIB="
    rem Prefer the final combined archive and deliberately ignore the implementation-only archive.
    for /r "%BUILDDIR%" %%P in (*ps2_drivers*.a) do (
        if /i not "%%~nxP"=="libps2_drivers_impl.a" (
            if not defined BUILT_LIB set "BUILT_LIB=%%~fP"
        )
    )
    if defined BUILT_LIB (
        echo.
        echo CMake did not install libps2_drivers.a; copying:
        echo   !BUILT_LIB!
        if not exist "%DEST%\lib" mkdir "%DEST%\lib"
        copy /Y "!BUILT_LIB!" "%DEST%\lib\libps2_drivers.a" >nul
        if errorlevel 1 (
            echo ERROR: Failed to copy libps2_drivers.a.
            goto :fail
        )
    ) else (
        echo.
        echo ERROR: CMake reported a successful ps2_drivers build but no final combined
        echo        ps2_drivers archive could be found. The archive listing above is authoritative.
        goto :fail
    )
)

echo.
echo Writing PS2Build package metadata...
if exist "!SRCDIR!\package.yaml" (
    echo Using upstream PS2Build package metadata...
    copy /Y "!SRCDIR!\package.yaml" "%DEST%\package.yaml" >nul
    if errorlevel 1 (
        echo ERROR: Failed to copy upstream package.yaml.
        goto :fail
    )
) else (
    echo Upstream package.yaml not present; writing compatible fallback metadata...
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
if not exist "%DEST%\include\ps2_usbd_driver.h" (
    echo ERROR: Missing "%DEST%\include\ps2_usbd_driver.h".
    goto :fail
)
if not exist "%DEST%\include\ps2_fileXio_driver.h" (
    echo ERROR: Missing "%DEST%\include\ps2_fileXio_driver.h".
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
echo   include\ps2_usbd_driver.h
echo   include\ps2_fileXio_driver.h
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
