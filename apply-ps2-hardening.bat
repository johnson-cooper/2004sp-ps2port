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

if not exist "apply-ps2-hardening.ps1" (
  echo ERROR: apply-ps2-hardening.ps1 is missing.
  exit /b 1
)

echo Applying exact, fail-safe hardening edits...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0apply-ps2-hardening.ps1"
if errorlevel 1 (
  echo.
  echo ERROR: Hardening edits were not applied completely.
  echo No fuzzy patching was attempted. Read the error above.
  exit /b 1
)

echo.
where git >nul 2>nul
if not errorlevel 1 (
  git diff --check
  if errorlevel 1 exit /b 1
)

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

echo Installed PS2Build detected. Its local help follows:
echo ------------------------------------------------------------
ps2build --help

endlocal
