@echo off
setlocal
cd /d "%~dp0"

echo ============================================================
echo  PS2 visibility + sparse Ground hardware test
echo ============================================================
echo.

where git >nul 2>nul
if errorlevel 1 (
    echo ERROR: git was not found on PATH.
    exit /b 1
)

if not exist "ps2-visibility-sparse-ground.patch" (
    echo ERROR: ps2-visibility-sparse-ground.patch is missing.
    exit /b 1
)

rem If reverse-check succeeds, the patch is already present in the working tree.
git apply --reverse --check "ps2-visibility-sparse-ground.patch" >nul 2>nul
if not errorlevel 1 (
    echo Patch is already applied. Nothing to do.
    goto :ready
)

echo Checking patch against the current branch...
git apply --check "ps2-visibility-sparse-ground.patch"
if errorlevel 1 (
    echo.
    echo ERROR: Patch does not cleanly apply.
    echo Make sure you are on ps2-hardware-integration and have pulled the latest branch.
    echo No source files were changed.
    exit /b 1
)

echo Applying patch...
git apply "ps2-visibility-sparse-ground.patch"
if errorlevel 1 (
    echo ERROR: git apply failed.
    exit /b 1
)

echo Checking resulting diff...
git diff --check
if errorlevel 1 (
    echo ERROR: git diff --check found a problem.
    exit /b 1
)

:ready
echo.
echo Source is ready for the next LOCAL PS2Build build.
echo This test intentionally does NOT change:
echo   - the proven contiguous scene allocator
 echo  - network/IRX setup
 echo  - PS2_RENDER_RADIUS
 echo  - PS2 terrain materialisation window
 echo  - compiler optimization level
 echo.
echo Expected effects:
echo   - remove the PS2 permanent visibility matrix (~650 KiB)
echo   - avoid its temporary construction matrix (~790 KiB during init)
echo   - tolerate NULL Ground cells in sparse-scene loc traversal
 echo  - reject temporaryLocs overflow / Ground allocation failure cleanly
 echo.
echo Build with your normal PS2Build workflow, then test on real hardware.
exit /b 0
