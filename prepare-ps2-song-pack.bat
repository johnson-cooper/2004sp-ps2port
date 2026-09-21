@echo off
setlocal EnableExtensions
cd /d "%~dp0"

if "%~1"=="" goto :usage
if "%~2"=="" goto :usage

set "ID=%~1"
set "NAME=%~2"
if "%~3"=="" (
    set "SF2=rom\SCC1_Florestan.sf2"
) else (
    set "SF2=%~3"
)
set "MIDI=rom\cache\client\songs\%NAME%.mid"
set "OUT=build\bin\rom\ps2audio\%ID%.ps2m"

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
echo  2004sp PS2 - accurate SoundFont song pack
echo ============================================================
echo MIDI ID:   %ID%
echo Song:      %NAME%
echo Input:     %MIDI%
echo Output:    %OUT%
echo.

py -3 tools\ps2_audio\build_title_music.py "%SF2%" "%MIDI%" "%OUT%"
if errorlevel 1 exit /b 1

echo.
echo Accurate pack staged:
echo   %OUT%
echo.
echo Runtime will prefer this .ps2m for MIDI ID %ID%.
echo If it is missing or invalid, the compact MIDI player remains the fallback.
exit /b 0

:usage
echo Usage:
echo   prepare-ps2-song-pack.bat MIDI_ID SONG_FILENAME_WITHOUT_MID [SOUNDFONT.sf2]
echo.
echo First hardware target:
echo   prepare-ps2-song-pack.bat 106 expanse
exit /b 1
