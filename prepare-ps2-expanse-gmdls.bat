@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "ALT_DIR=build\diagnostics"
set "ALT_SF2=%ALT_DIR%\Older RuneScape.sf2"
set "ALT_URL=https://raw.githubusercontent.com/CypherNL/OSRS-MIDI-Player/2f1d3053ce2f31119694f6df2a1ed185abb910f8/soundbanks/Older%%20RuneScape.sf2"

if not exist "%ALT_DIR%" mkdir "%ALT_DIR%"

if not exist "%ALT_SF2%" (
    echo ============================================================
    echo  Downloading pinned GM.DLS-derived diagnostic SoundFont
    echo ============================================================
    echo Source:
    echo   CypherNL/OSRS-MIDI-Player
    echo Commit:
    echo   2f1d3053ce2f31119694f6df2a1ed185abb910f8
    echo.
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
      "$ProgressPreference='SilentlyContinue'; Invoke-WebRequest -UseBasicParsing -Uri '%ALT_URL%' -OutFile '%ALT_SF2%'"
    if errorlevel 1 (
        echo ERROR: Failed to download diagnostic SoundFont.
        exit /b 1
    )
)

if not exist "%ALT_SF2%" (
    echo ERROR: Missing "%ALT_SF2%".
    exit /b 1
)

echo.
echo ============================================================
echo  2004sp PS2 - Expanse GM.DLS SoundFont A/B
echo ============================================================
echo This changes only:
echo   build\bin\rom\ps2audio\106.ps2m
echo.
echo client.elf and rs2midi.irx are not rebuilt or modified.
echo.

call prepare-ps2-song-pack.bat 106 expanse "%ALT_SF2%"
if errorlevel 1 exit /b 1

echo.
echo ============================================================
echo  A/B pack ready
echo ============================================================
echo Test the newly generated:
echo   build\bin\rom\ps2audio\106.ps2m
echo.
echo To restore the normal SCC1 pack:
echo   prepare-ps2-song-pack.bat 106 expanse
echo.
exit /b 0
