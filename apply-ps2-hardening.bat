@echo off
setlocal EnableExtensions
cd /d "%~dp0"

echo ============================================================
echo  2004sp PS2 - hardening test preparation
echo ============================================================
echo.

if not exist "ps2.yaml" (
  echo ERROR: Run this from the repository root.
  exit /b 1
)

where git >nul 2>nul
if errorlevel 1 (
  echo ERROR: git.exe is required to apply the source patch.
  exit /b 1
)

git apply --reverse --check ps2-hardening-1.patch >nul 2>nul
if not errorlevel 1 (
  echo Hardening patch is already applied locally.
  goto :buildinfo
)

echo Checking patch against the current checkout...
git apply --check ps2-hardening-1.patch
if errorlevel 1 (
  echo.
  echo ERROR: Patch does not apply cleanly. Do not force it.
  echo Make sure you checked out ps2-audit-hardening-1 and your source
  echo files do not contain unrelated local edits.
  exit /b 1
)

echo Applying hardening patch locally...
git apply ps2-hardening-1.patch
if errorlevel 1 exit /b 1

:buildinfo
echo.
echo Patch status:
git diff --check
if errorlevel 1 exit /b 1

echo.
echo Source hardening is ready for your LOCAL PS2Build build.
echo ps2.yaml remains at -O1 and the embedded IRX order is unchanged.
echo Expected ELF after your normal PS2Build command:
echo   build\bin\client.elf
echo.
where ps2build >nul 2>nul
if errorlevel 1 (
  echo ps2build is not currently on PATH in this shell.
  echo Start the same PS2Build environment you normally use for this repo.
  exit /b 0
)

echo Installed PS2Build detected. Its local help follows so the command
echo comes from your installed version instead of being guessed here:
echo ------------------------------------------------------------
ps2build --help

endlocal
