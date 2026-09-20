param(
    [Parameter(Mandatory=$true)]
    [string]$GoodElf,

    [Parameter(Mandatory=$true)]
    [string]$TestElf,

    [Parameter(Mandatory=$false)]
    [string]$Ps2BuildRoot = $env:PS2DEV,

    [Parameter(Mandatory=$false)]
    [string]$OutFile = "elf-layout-report.txt"
)

$ErrorActionPreference = "Stop"

function Resolve-Tool([string]$name) {
    $candidates = @()
    if ($Ps2BuildRoot) {
        $candidates += Join-Path $Ps2BuildRoot ("toolchain\ee\bin\mips64r5900el-ps2-elf-" + $name + ".exe")
        $candidates += Join-Path $Ps2BuildRoot ("toolchain\ee\bin\mips64r5900el-ps2-elf-" + $name)
    }

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    $cmd = Get-Command ("mips64r5900el-ps2-elf-" + $name) -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    throw "Could not find mips64r5900el-ps2-elf-$name. Pass -Ps2BuildRoot with the folder containing toolchain\ee\bin."
}

function Invoke-NativeTool([string]$tool, [string[]]$arguments, [bool]$AllowFailure = $false) {
    # Windows PowerShell 5 turns native stderr into ErrorRecord objects. With
    # $ErrorActionPreference="Stop", tools such as nm can abort the script
    # before we get a chance to inspect $LASTEXITCODE. Temporarily use
    # Continue so stripped-ELF diagnostics ("no symbols") can be captured.
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $output = & $tool @arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldPreference
    }

    $text = ($output | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
    if ($exitCode -ne 0 -and !$AllowFailure) {
        throw ("{0} failed with exit code {1}{2}{3}" -f $tool, $exitCode, [Environment]::NewLine, $text)
    }

    return [PSCustomObject]@{
        ExitCode = $exitCode
        Text = $text
    }
}

function Run-Tool([string]$tool, [string[]]$arguments) {
    return (Invoke-NativeTool $tool $arguments $false).Text
}

function File-Info([string]$path) {
    $item = Get-Item $path
    $hash = (Get-FileHash -Algorithm SHA256 $path).Hash
    return "Path: $($item.FullName)
Bytes: $($item.Length)
SHA256: $hash"
}

if (!(Test-Path $GoodElf)) { throw "Good ELF not found: $GoodElf" }
if (!(Test-Path $TestElf)) { throw "Test ELF not found: $TestElf" }

$readelf = Resolve-Tool "readelf"
$nm      = Resolve-Tool "nm"
$size    = Resolve-Tool "size"
$objdump = Resolve-Tool "objdump"

$good = (Resolve-Path $GoodElf).Path
$test = (Resolve-Path $TestElf).Path

$symbols = @(
    "errno",
    "_end",
    "end",
    "__bss_start",
    "_gp",
    "_ftext",
    "_etext",
    "_fdata",
    "_edata",
    "ps2_audio_ready",
    "ps2_crash_client",
    "platform_init",
    "clientstream_opensocket",
    "clientstream_read",
    "clientstream_write"
)

function Symbol-Report([string]$elf) {
    $nmResult = Invoke-NativeTool $nm @("-n", "-S", $elf) $true

    # PS2Build strips the final client.elf, so nm legitimately exits non-zero
    # with "no symbols". That is not a bad ELF and should not abort the layout
    # comparison. Section/program-header data remains fully useful.
    if ($nmResult.ExitCode -ne 0) {
        if ($nmResult.Text -match "no symbols") {
            return "<stripped ELF: no symbol table available>"
        }
        return ("<nm unavailable for this ELF, exit={0}>{1}{2}" -f $nmResult.ExitCode, [Environment]::NewLine, $nmResult.Text)
    }

    $lines = $nmResult.Text -split "\r?\n"
    $picked = @()
    foreach ($symbol in $symbols) {
        $match = $lines | Where-Object { $_ -match ("\s" + [regex]::Escape($symbol) + "$") } | Select-Object -First 1
        if ($match) {
            $picked += $match
        } else {
            $picked += ("<missing> " + $symbol)
        }
    }
    return ($picked -join [Environment]::NewLine)
}

function Section-Report([string]$elf) {
    $raw = Run-Tool $readelf @("-SW", $elf)
    $wanted = $raw -split "?
" | Where-Object {
        $_ -match "\.(text|rodata|data|sdata|bss|sbss|ctors|dtors|reginfo|mdebug)" -or
        $_ -match "Section Headers:"
    }
    return ($wanted -join [Environment]::NewLine)
}

$report = New-Object System.Text.StringBuilder
[void]$report.AppendLine("PS2 ELF layout comparison")
[void]$report.AppendLine("=========================")
[void]$report.AppendLine()
[void]$report.AppendLine("GOOD ELF")
[void]$report.AppendLine("--------")
[void]$report.AppendLine((File-Info $good))
[void]$report.AppendLine()
[void]$report.AppendLine("TEST ELF")
[void]$report.AppendLine("--------")
[void]$report.AppendLine((File-Info $test))
[void]$report.AppendLine()
[void]$report.AppendLine("SIZE -A: GOOD")
[void]$report.AppendLine((Run-Tool $size @("-A", $good)))
[void]$report.AppendLine()
[void]$report.AppendLine("SIZE -A: TEST")
[void]$report.AppendLine((Run-Tool $size @("-A", $test)))
[void]$report.AppendLine()
[void]$report.AppendLine("SELECTED SECTIONS: GOOD")
[void]$report.AppendLine((Section-Report $good))
[void]$report.AppendLine()
[void]$report.AppendLine("SELECTED SECTIONS: TEST")
[void]$report.AppendLine((Section-Report $test))
[void]$report.AppendLine()
[void]$report.AppendLine("PROGRAM HEADERS: GOOD")
[void]$report.AppendLine((Run-Tool $readelf @("-lW", $good)))
[void]$report.AppendLine()
[void]$report.AppendLine("PROGRAM HEADERS: TEST")
[void]$report.AppendLine((Run-Tool $readelf @("-lW", $test)))
[void]$report.AppendLine()
[void]$report.AppendLine("KEY SYMBOLS: GOOD")
[void]$report.AppendLine((Symbol-Report $good))
[void]$report.AppendLine()
[void]$report.AppendLine("KEY SYMBOLS: TEST")
[void]$report.AppendLine((Symbol-Report $test))
[void]$report.AppendLine()
[void]$report.AppendLine("OBJDUMP HEADERS: GOOD")
[void]$report.AppendLine((Run-Tool $objdump @("-h", $good)))
[void]$report.AppendLine()
[void]$report.AppendLine("OBJDUMP HEADERS: TEST")
[void]$report.AppendLine((Run-Tool $objdump @("-h", $test)))

$report.ToString() | Set-Content -Encoding UTF8 $OutFile
Write-Host "Wrote $OutFile"
Write-Host ""
Write-Host "Quick checks:"
Write-Host "  1. Compare SHA256 first. If identical, the binaries are byte-for-byte identical."
Write-Host "  2. Compare .text/.rodata/.data/.bss addresses and sizes."
Write-Host "  3. Final PS2Build ELFs may be stripped; KEY SYMBOLS will say so instead of failing."
Write-Host "  4. Compare the end address of the last allocated section/LOAD segment as the stripped-ELF proxy for _end."
Write-Host "  5. If that end moves, the initial heap boundary may move too."
