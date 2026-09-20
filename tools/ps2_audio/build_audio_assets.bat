@echo off
setlocal EnableExtensions
cd /d "%~dp0\..\.."

if "%~1"=="" (
    echo Usage:
    echo   tools\ps2_audio\build_audio_assets.bat "path\to\soundfont.sf2" ["path\to\midi-folder"] ["output-folder"]
    exit /b 1
)

set "SF2=%~1"
set "MIDIDIR=%~2"
if "%~3"=="" (
    set "OUT=build\audio"
) else (
    set "OUT=%~3"
)

where py >nul 2>nul
if errorlevel 1 (
    echo ERROR: Python launcher "py" was not found.
    exit /b 1
)

echo ============================================================
echo  2004sp PS2 audio asset build
echo ============================================================
echo SoundFont: "%SF2%"
echo Output:    "%OUT%"
echo.

py -3 tools\ps2_audio\sf2_to_ps2bank.py "%SF2%" "%OUT%\soundfont"
if errorlevel 1 exit /b 1

if not "%MIDIDIR%"=="" (
    if not exist "%OUT%\songs" mkdir "%OUT%\songs"
    for %%F in ("%MIDIDIR%\*.mid") do (
        echo.
        echo Compiling %%~nxF
        py -3 tools\ps2_audio\midi_to_ps2seq.py "%%~fF" "%OUT%\songs\%%~nF.rseq"
        if errorlevel 1 exit /b 1
    )
)

echo.
echo Audio assets built successfully.
exit /b 0
