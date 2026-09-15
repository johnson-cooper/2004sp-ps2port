@echo off
setlocal EnableExtensions
cd /d "%~dp0"

echo ============================================================
echo  2004sp-ps2port - LOCAL PS2 build helper
echo ============================================================
echo.
echo This helper intentionally DOES NOT use GitHub Actions and does
echo not commit or push anything.
echo.

if not exist "ps2.yaml" (
  echo ERROR: ps2.yaml was not found.
  echo Put this BAT in the repository root and run it there.
  exit /b 1
)

where ps2build >nul 2>nul
if errorlevel 1 (
  echo ERROR: "ps2build" is not available on PATH.
  echo.
  echo Install/start the PS2Build environment documented at:
  echo https://ps2.techwritescode.dev/
  echo.
  echo I am deliberately not guessing a substitute PS2SDK command,
  echo because this repository is configured through ps2.yaml.
  exit /b 1
)

echo PS2Build found:
where ps2build
echo.

rem PS2Build's exact CLI can vary by installed release. Show the
rem locally installed tool's authoritative syntax instead of guessing.
echo ---- Installed PS2Build help ----
ps2build --help
echo.
echo ------------------------------------------------------------
echo Run the build subcommand shown above for ps2.yaml.
echo After it succeeds, verify/stage:
echo   build\bin\client.elf
echo   build\bin\rom\
echo   build\bin\rom\config.ini
echo and any runtime files required by your installed PS2Build release.
echo.
echo IMPORTANT: Do not replace the embedded IRX setup from ps2.yaml.
echo ============================================================

endlocal
