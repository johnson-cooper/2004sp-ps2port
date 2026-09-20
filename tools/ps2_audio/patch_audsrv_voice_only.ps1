param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir
)

$ErrorActionPreference = "Stop"
$source = Join-Path $SourceDir "iop\sound\audsrv\src\audsrv.c"

if (-not (Test-Path -LiteralPath $source)) {
    throw "audsrv source file not found: $source"
}

$text = [System.IO.File]::ReadAllText($source)

$pattern = '(?s)int audsrv_init\(\)\s*\{.*?\n\}\n\n/\*\* Returns the number of bytes'
$matches = [regex]::Matches($text, $pattern)
if ($matches.Count -ne 1) {
    throw "Expected exactly one audsrv_init() body, found $($matches.Count). Refusing to patch an unknown audsrv source layout."
}

$replacement = @'
int audsrv_init()
{
	/*
	 * 2004sp PS2 voice-only mode.
	 *
	 * Stock audsrv starts a permanent PCM streaming worker and a looping
	 * sceSdBlockTrans DMA even when the application only uses ADPCM voices.
	 * Real PS2 hardware proved that the stock initialization can leave the
	 * title screen alive while breaking the later RuneScape server connection.
	 *
	 * RuneScape music/SFX are being converted offline to SPU2 ADPCM, so we
	 * only need libsd/SPU2 initialization here. Keep the RPC service and all
	 * ADPCM upload/playback APIs, but do not create the streaming semaphores,
	 * transfer callback, block DMA, format converter, or play thread.
	 */
	if (initialized)
	{
		return 0;
	}

	if (sceSdInit(SD_INIT_COLD) < 0)
	{
		DPRINTF("failed to initialize libsd\n");
		return -1;
	}

	readpos = 0;
	writepos = 0;
	playing = 0;

	/* ADPCM voices use core 1. Give that core normal master output; individual
	 * voice volume/pan is still controlled by audsrv_adpcm_set_volume(). */
	sceSdSetParam(SD_CORE_1 | SD_PARAM_MVOLL, MAX_VOLUME);
	sceSdSetParam(SD_CORE_1 | SD_PARAM_MVOLR, MAX_VOLUME);

	initialized = 1;
	return AUDSRV_ERR_NOERROR;
}

/** Returns the number of bytes
'@

$patched = [regex]::Replace($text, $pattern, $replacement, 1)

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($source, $patched, $utf8NoBom)

Write-Host "Patched audsrv for 2004sp ADPCM-only mode:"
Write-Host "  $source"
Write-Host "Disabled: PCM streaming thread + looping block DMA"
Write-Host "Kept:     RPC + libsd/SPU2 + ADPCM sample/channel playback"
