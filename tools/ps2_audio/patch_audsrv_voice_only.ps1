param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir
)

$ErrorActionPreference = "Stop"
$SourceDir = $SourceDir.Trim()
$SourceDir = $SourceDir.Trim('"')
$SourceDir = $SourceDir.TrimEnd('\', '/')

function Read-Text([string]$RelativePath) {
    $path = Join-Path $SourceDir $RelativePath
    if (-not (Test-Path -LiteralPath $path)) { throw "audsrv source file not found: $path" }
    return [System.IO.File]::ReadAllText($path)
}
function Write-Text([string]$RelativePath, [string]$Text) {
    $path = Join-Path $SourceDir $RelativePath
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($path, $Text, $utf8NoBom)
}
function Replace-LiteralOnce([string]$Text,[string]$Needle,[string]$Replacement,[string]$Description) {
    $first = $Text.IndexOf($Needle)
    if ($first -lt 0) { throw "Could not locate $Description in pinned audsrv source." }
    if ($Text.IndexOf($Needle, $first + $Needle.Length) -ge 0) { throw "Found $Description more than once." }
    return $Text.Substring(0,$first) + $Replacement + $Text.Substring($first + $Needle.Length)
}
function Replace-RegexOnce([string]$Text,[string]$Pattern,[string]$Replacement,[string]$Description) {
    $matches = [regex]::Matches($Text,$Pattern)
    if ($matches.Count -ne 1) { throw "Expected one $Description, found $($matches.Count)." }
    return [regex]::Replace($Text,$Pattern,$Replacement,1)
}

# Keep the real-hardware-proven voice-only initialization.
$audsrvCPath = "iop\sound\audsrv\src\audsrv.c"
$text = Read-Text $audsrvCPath
$pattern = '(?s)int audsrv_init\(\)\s*\{.*?\n\}\n\n/\*\* Returns the number of bytes'
$matches = [regex]::Matches($text,$pattern)
if ($matches.Count -ne 1) { throw "Expected exactly one audsrv_init() body, found $($matches.Count)." }
$replacement = @'
int audsrv_init()
{
	/*
	 * 2004sp PS2 voice-only mode.
	 * RuneScape music/SFX are converted offline to SPU2 ADPCM. Do not start
	 * audsrv's permanent PCM stream worker, semaphores or looping block DMA:
	 * real hardware proved that path breaks the later network connection.
	 */
	if (initialized)
		return 0;

	if (sceSdInit(SD_INIT_COLD) < 0)
	{
		DPRINTF("failed to initialize libsd\n");
		return -1;
	}

	readpos = 0;
	writepos = 0;
	playing = 0;
	sceSdSetParam(SD_CORE_1 | SD_PARAM_MVOLL, MAX_VOLUME);
	sceSdSetParam(SD_CORE_1 | SD_PARAM_MVOLR, MAX_VOLUME);
	initialized = 1;
	return AUDSRV_ERR_NOERROR;
}

/** Returns the number of bytes
'@
$text = [regex]::Replace($text,$pattern,$replacement,1)
Write-Text $audsrvCPath $text

# Private RPC command IDs + IOP declarations.
$iopHeaderPath = "iop\sound\audsrv\include\audsrv.h"
$text = Read-Text $iopHeaderPath
$newMarker = @'
#define AUDSRV_IS_ADPCM_PLAYING     0x001d

/* 2004sp private voice-only extensions. */
#define AUDSRV_RS2_PLAY_ADPCM        0x0020
#define AUDSRV_RS2_KEY_OFF           0x0021
#define AUDSRV_RS2_SET_PITCH         0x0022
'@
$text = Replace-LiteralOnce $text "#define AUDSRV_IS_ADPCM_PLAYING     0x001d" $newMarker "IOP RPC command marker"
$newProto = @'
extern int audsrv_is_adpcm_playing(int ch, u32 id);
extern int audsrv_rs2_ch_play_adpcm(int ch, u32 id, int pitch);
extern int audsrv_rs2_key_off(int ch);
extern int audsrv_rs2_set_pitch(int ch, int pitch);
'@
$text = Replace-LiteralOnce $text "extern int audsrv_is_adpcm_playing(int ch, u32 id);" $newProto "IOP ADPCM prototype marker"
Write-Text $iopHeaderPath $text

# IOP-side controls. Explicit play owns/restarts a selected SPU2 voice.
$adpcmPath = "iop\sound\audsrv\src\adpcm.c"
$text = Read-Text $adpcmPath
$marker = "/** Initializes adpcm unit of audsrv"
$voiceCode = @'
/* 2004sp private MIDI/SPU2 helpers. */
int audsrv_rs2_ch_play_adpcm(int ch, u32 id, int pitch)
{
	adpcm_list_t *a;
	if (ch < 0 || ch >= 24)
		return -AUDSRV_ERR_ARGS;
	a = adpcm_loaded(id);
	if (a == NULL)
		return -AUDSRV_ERR_ARGS;
	if (pitch <= 0)
		pitch = a->pitch;
	if (pitch < 1)
		pitch = 1;
	if (pitch > 0x3fff)
		pitch = 0x3fff;

	sceSdSetSwitch(SD_CORE_1 | SD_SWITCH_KOFF, (1 << ch));
	sceSdSetParam(SD_CORE_1 | (ch << 1) | SD_VPARAM_ADSR1, 0x80ff);
	sceSdSetParam(SD_CORE_1 | (ch << 1) | SD_VPARAM_ADSR2, 0x4000);
	sceSdSetParam(SD_CORE_1 | (ch << 1) | SD_VPARAM_PITCH, pitch);
	sceSdSetAddr(SD_CORE_1 | (ch << 1) | SD_VOICE_START, a->spu2_addr);
	sceSdSetSwitch(SD_CORE_1 | SD_SWITCH_KON, (1 << ch));
	return ch;
}

int audsrv_rs2_key_off(int ch)
{
	if (ch < 0 || ch >= 24)
		return -AUDSRV_ERR_ARGS;
	sceSdSetSwitch(SD_CORE_1 | SD_SWITCH_KOFF, (1 << ch));
	return AUDSRV_ERR_NOERROR;
}

int audsrv_rs2_set_pitch(int ch, int pitch)
{
	if (ch < 0 || ch >= 24)
		return -AUDSRV_ERR_ARGS;
	if (pitch < 1)
		pitch = 1;
	if (pitch > 0x3fff)
		pitch = 0x3fff;
	sceSdSetParam(SD_CORE_1 | (ch << 1) | SD_VPARAM_PITCH, pitch);
	return AUDSRV_ERR_NOERROR;
}

'@
$text = Replace-LiteralOnce $text $marker ($voiceCode + $marker) "IOP ADPCM init marker"
Write-Text $adpcmPath $text

# IOP RPC dispatcher.
$rpcServerPath = "iop\sound\audsrv\src\rpc_server.c"
$text = Read-Text $rpcServerPath
$pattern = '(?ms)^(\s*)case AUDSRV_IS_ADPCM_PLAYING:\s*\r?\n\s*ret = audsrv_is_adpcm_playing\(data\[0\], data\[1\]\);\s*\r?\n\s*break;'
$replacement = @'
		case AUDSRV_IS_ADPCM_PLAYING:
			ret = audsrv_is_adpcm_playing(data[0], data[1]);
			break;

		case AUDSRV_RS2_PLAY_ADPCM:
			ret = audsrv_rs2_ch_play_adpcm(data[0], data[1], data[2]);
			break;

		case AUDSRV_RS2_KEY_OFF:
			ret = audsrv_rs2_key_off(data[0]);
			break;

		case AUDSRV_RS2_SET_PITCH:
			ret = audsrv_rs2_set_pitch(data[0], data[1]);
			break;
'@
$text = Replace-RegexOnce $text $pattern $replacement "IOP RPC ADPCM dispatch marker"
Write-Text $rpcServerPath $text

# EE declarations and wrappers.
$eeHeaderPath = "ee\rpc\audsrv\include\audsrv.h"
$text = Read-Text $eeHeaderPath
$newEeProto = @'
extern int audsrv_is_adpcm_playing(int ch, audsrv_adpcm_t *adpcm);

/* 2004sp private voice-only extensions used by the PS2 MIDI sequencer. */
extern int audsrv_rs2_ch_play_adpcm(int ch, audsrv_adpcm_t *adpcm, int pitch);
extern int audsrv_rs2_key_off(int ch);
extern int audsrv_rs2_set_pitch(int ch, int pitch);
'@
$text = Replace-LiteralOnce $text "extern int audsrv_is_adpcm_playing(int ch, audsrv_adpcm_t *adpcm);" $newEeProto "EE ADPCM prototype marker"
Write-Text $eeHeaderPath $text

$eeRpcPath = "ee\rpc\audsrv\src\audsrv_rpc.c"
$text = Read-Text $eeRpcPath
$pattern = '(?ms)^int audsrv_is_adpcm_playing\(int ch, audsrv_adpcm_t \*adpcm\)\s*\r?\n\{\s*\r?\n\s*return call_rpc_2\(AUDSRV_IS_ADPCM_PLAYING, ch, \(u32\)adpcm\);\s*\r?\n\}'
$replacement = @'
int audsrv_is_adpcm_playing(int ch, audsrv_adpcm_t *adpcm)
{
	return call_rpc_2(AUDSRV_IS_ADPCM_PLAYING, ch, (u32)adpcm);
}

int audsrv_rs2_ch_play_adpcm(int ch, audsrv_adpcm_t *adpcm, int pitch)
{
	return call_rpc_3(AUDSRV_RS2_PLAY_ADPCM, ch, (u32)adpcm, pitch);
}

int audsrv_rs2_key_off(int ch)
{
	return call_rpc_1(AUDSRV_RS2_KEY_OFF, ch);
}

int audsrv_rs2_set_pitch(int ch, int pitch)
{
	return call_rpc_2(AUDSRV_RS2_SET_PITCH, ch, pitch);
}
'@
$text = Replace-RegexOnce $text $pattern $replacement "EE ADPCM RPC wrapper marker"
Write-Text $eeRpcPath $text

Write-Host "Patched audsrv for 2004sp voice-only MIDI mode:"
Write-Host "  disabled: PCM streaming thread + looping block DMA"
Write-Host "  kept:     RPC + libsd/SPU2 + ADPCM uploads/playback"
Write-Host "  added:    explicit-pitch play + key-off + live pitch RPCs"
