@echo off
setlocal EnableExtensions
cd /d "%~dp0"

where py >nul 2>nul
if errorlevel 1 (
    echo ERROR: Python launcher "py" was not found.
    exit /b 1
)

if not exist "build\bin" (
    echo ERROR: build\bin does not exist. Run ps2build build first.
    exit /b 1
)

echo ============================================================
echo  2004sp PS2 - build all available rev254 accurate song packs
echo ============================================================
echo.
py -3 tools\ps2_audio\build_all_rev254_packs.py --keep-going
set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" (
    echo One or more packs failed. Successful packs remain staged.
)
exit /b %RC%
