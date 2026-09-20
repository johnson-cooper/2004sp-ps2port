@echo off
setlocal EnableExtensions
cd /d "%~dp0"

if not exist "rom\SCC1_Florestan.sf2" (
    echo ERROR: rom\SCC1_Florestan.sf2 was not found.
    exit /b 1
)

where py >nul 2>nul
if errorlevel 1 (
    echo ERROR: Python launcher "py" was not found.
    exit /b 1
)

echo ============================================================
echo  Building compact PS2 MIDI bank from SCC1_Florestan.sf2
echo ============================================================
py -3 tools\ps2_audio\build_compact_midi_bank.py
if errorlevel 1 exit /b 1

echo.
echo Generated src\platform\ps2_midi_bank.c
echo Now run:
echo   ps2build build
echo.
echo rs2midi.irx does not need to change for sample-only bank updates.
endlocal
