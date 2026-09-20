@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "SF2=rom\SCC1_Florestan.sf2"
set "MIDI=rom\cache\client\songs\scape_main.mid"
set "OUT=build\bin\rom\ps2audio\scape_main.ps2m"

where py >nul 2>nul
if errorlevel 1 (
    echo ERROR: Python launcher "py" was not found.
    exit /b 1
)

if not exist "%SF2%" (
    echo ERROR: Missing "%SF2%".
    exit /b 1
)
if not exist "%MIDI%" (
    echo ERROR: Missing "%MIDI%".
    exit /b 1
)
if not exist "build\bin" (
    echo ERROR: build\bin does not exist. Run ps2build build first.
    exit /b 1
)

echo ============================================================
echo  2004sp PS2 - build title music
echo ============================================================
echo.
py -3 tools\ps2_audio\build_title_music.py "%SF2%" "%MIDI%" "%OUT%"
if errorlevel 1 exit /b 1

echo.
echo Title music staged:
echo   %OUT%
echo.
echo Copy/stage build\bin normally for the hardware test.
exit /b 0
