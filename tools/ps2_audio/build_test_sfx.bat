@echo off
setlocal

set "CONTENT=%~1"
if "%CONTENT%"=="" set "CONTENT=..\LostCity-Content"

set "SYNTH=%CONTENT%\synth\skills\smithing\anvil_4.synth"
set "TOOL=build\tools\export_synth_wav.exe"
set "WAV=build\diagnostics\anvil_4.wav"
set "OUT=build\bin\rom\ps2sfx\468.ps2a"

if not exist "bin\tcc\tcc.exe" (
    echo ERROR: bin\tcc\tcc.exe not found.
    exit /b 1
)

if not exist "%SYNTH%" (
    echo ERROR: anvil_4.synth not found:
    echo   %SYNTH%
    echo.
    echo Usage:
    echo   tools\ps2_audio\build_test_sfx.bat C:\path\to\LostCity-Content
    exit /b 1
)

if not exist "build\tools" mkdir "build\tools"
if not exist "build\diagnostics" mkdir "build\diagnostics"
if not exist "build\bin\rom\ps2sfx" mkdir "build\bin\rom\ps2sfx"

echo ============================================================
echo 2004sp PS2 SFX proof - anvil_4 / synth id 468
echo ============================================================
echo.

echo [1/2] Exporting rev254 synth to WAV...
"bin\tcc\tcc.exe" -Isrc -o "%TOOL%" ^
    tools\ps2_audio\export_synth_wav.c ^
    src\sound\wave.c ^
    src\sound\tone.c ^
    src\sound\envelope.c
if errorlevel 1 exit /b 1

"%TOOL%" "%SYNTH%" "%WAV%" 1
if errorlevel 1 exit /b 1

echo.
echo [2/2] Encoding WAV to PS2 SPU2 ADPCM...
py -3 tools\ps2_audio\wav_to_ps2_adpcm.py "%WAV%" "%OUT%"
if errorlevel 1 exit /b 1

echo.
echo ============================================================
echo Done.
echo No server patch or PS2M remap is used by this proof.
echo Copy this file with the normal build to your USB:
echo   rom\ps2sfx\468.ps2a
echo ============================================================

endlocal
