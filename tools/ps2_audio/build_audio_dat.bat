@echo off
setlocal

set "CONTENT=%~1"
if "%CONTENT%"=="" set "CONTENT=..\LostCity-Content"

echo ============================================================
echo 2004sp PS2 unified audio.dat builder
echo ============================================================
echo Content: %CONTENT%
echo Music:   build\bin\rom\ps2audio
echo Output:  build\bin\rom\audio.dat
echo.

py -3 tools\ps2_audio\build_audio_dat.py ^
    --content "%CONTENT%" ^
    --music-dir "build\bin\rom\ps2audio" ^
    --output "build\bin\rom\audio.dat"

if errorlevel 1 (
    echo.
    echo ERROR: audio.dat build failed.
    exit /b 1
)

echo.
echo ============================================================
echo audio.dat is ready.
echo.
echo Final USB audio files:
echo   rom\audio.dat
echo   rs2midi.irx
echo.
echo The old rom\ps2audio and rom\ps2sfx folders may be omitted
echo after audio.dat passes the real-hardware acceptance test.
echo ============================================================

endlocal
