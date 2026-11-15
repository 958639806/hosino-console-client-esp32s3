#requires -Version 5.1
<#
.SYNOPSIS
Checks a firmware image, creates or verifies a complete 16 MB backup, then flashes it.
.DESCRIPTION
Variant and Port must be supplied explicitly. DryRun performs local checks and
prints the plan without invoking Python, opening a serial port, or writing files.
The 40/80 variants select the display QSPI clock; this script does not change the
SPI flash clock. Factory restores the published recovery image, not user settings.
After backup, the device stays in its flasher stub for the write stage. This
sequence completed on this board's native USB COM9 connection on 2026-11-11.
ExistingBackup reuses a local 16 MB image only when its .sha256 sidecar matches;
this skips read_flash and uses default_reset before the write stage.
Localized and Apps validate and write bootloader, partition table, and application
separately. Their NVS/PHY flash range (0x9000-0xFFFF) is not written. Other families
retain merged-image writes, which overwrite intervening gaps including NVS.
Recovery from interrupted connections was not tested. If a later local check fails, reset the board
manually to leave the stub. Only a successful write requests the final hard reset.
.EXAMPLE
.\scripts\Flash-Firmware.ps1 -Variant 80 -Port COM7 -DryRun
.EXAMPLE
.\scripts\Flash-Firmware.ps1 -Variant factory -Port COM7
.EXAMPLE
.\scripts\Flash-Firmware.ps1 -Localized -Variant 80 -Port COM9 -ExistingBackup .\backups\saved.bin -DryRun
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('40', '80', 'factory')]
    [string]$Variant,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^COM[1-9][0-9]*$')]
    [string]$Port,

    [switch]$DryRun,

    [switch]$Performance,

    [switch]$Buffered,

    [switch]$Corners,

    [switch]$Localized,

    [switch]$Apps,

    [ValidateNotNullOrEmpty()]
    [string]$ExistingBackup,

    [ValidateNotNullOrEmpty()]
    [string]$Python = 'C:\esp\hosino-qspi\tools\python_env\idf5.5_py3.12_env\Scripts\python.exe'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
$flashBytes = [long]16777216
$Port = $Port.ToUpperInvariant()
$Variant = $Variant.ToLowerInvariant()
if ($Corners -and ($Localized -or $Apps)) {
    throw '-Corners is mutually exclusive with -Localized and -Apps.'
}
if (($Performance -or $Buffered -or $Corners -or $Localized -or $Apps) -and $Variant -eq 'factory') {
    throw 'Performance/Buffered/Corners/Localized/Apps select experimental images; use factory without those switches to restore.'
}

function Get-RequiredString {
    param([object]$Object, [string]$Name, [string]$Context)
    if ($null -eq $Object) { throw "Missing $Context." }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $property.Value -isnot [string] -or
        [string]::IsNullOrWhiteSpace($property.Value)) {
        throw "Missing or invalid $Context.$Name."
    }
    return $property.Value
}

function Get-RequiredSize {
    param([object]$Object, [string]$Context)
    $property = $Object.PSObject.Properties['size']
    if ($null -eq $property -or
        ($property.Value -isnot [int] -and $property.Value -isnot [long]) -or
        $property.Value -le 0 -or $property.Value -gt $flashBytes) {
        throw "Missing or invalid $Context.size."
    }
    return [long]$property.Value
}

function Format-Command {
    param([string]$Executable, [string[]]$Arguments)
    $quoted = @($Executable) + $Arguments | ForEach-Object {
        "'" + $_.Replace("'", "''") + "'"
    }
    return '& ' + ($quoted -join ' ')
}

function Invoke-Esptool {
    param([string[]]$Arguments, [string]$Operation)
    & $script:pythonExecutable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Operation failed (esptool exit code $LASTEXITCODE). Flashing stopped."
    }
}

if ($Variant -eq 'factory') {
    $manifestPath = Join-Path $workspace 'source-lock.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $recordProperty = $manifest.PSObject.Properties['factory_recovery']
    if ($null -eq $recordProperty) { throw 'Missing source-lock.json.factory_recovery.' }
    $record = $recordProperty.Value
    $filename = Get-RequiredString $record 'filename' 'factory_recovery'
    $expectedHash = Get-RequiredString $record 'sha256' 'factory_recovery'
    $imageDirectory = Join-Path $workspace 'firmware\recovery'
}
else {
    $family = if ($Apps) { 'apps' } elseif ($Localized) { 'localized' } elseif ($Corners) { 'corners' } elseif ($Buffered) { 'buffered' } elseif ($Performance) { 'performance' } else { 'comparison' }
    $imageDirectory = Join-Path $workspace "firmware\$family\${Variant}mhz"
    $manifestPath = Join-Path $imageDirectory 'manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $recordProperty = $manifest.PSObject.Properties['combined']
    if ($null -eq $recordProperty) { throw 'Missing firmware manifest.combined.' }
    $record = $recordProperty.Value
    $filename = Get-RequiredString $record 'file' 'combined'
    $expectedHash = Get-RequiredString $record 'sha256' 'combined'
}

# Manifests name a file inside their package, never an arbitrary filesystem path.
if ([System.IO.Path]::IsPathRooted($filename) -or $filename -match '[\\/:]' -or
    $filename -eq '.' -or $filename -eq '..' -or
    [System.IO.Path]::GetExtension($filename) -ine '.bin') {
    throw "Invalid firmware filename in $manifestPath."
}
if ($expectedHash -notmatch '^[0-9a-fA-F]{64}$') {
    throw "Invalid SHA256 in $manifestPath."
}
$imagePath = Join-Path $imageDirectory $filename
$image = Get-Item -LiteralPath $imagePath
if ($image.PSIsContainer -or $image.Length -le 0 -or $image.Length -gt $flashBytes) {
    throw 'Firmware must be a nonempty binary no larger than 16 MB.'
}
$actualHash = (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -ne $expectedHash.ToLowerInvariant()) {
    throw "Firmware SHA256 mismatch. Expected $expectedHash; got $actualHash. No device accessed."
}

$useSplitImages = $Localized -or $Apps
$writeImages = @([pscustomobject]@{ Path = $imagePath; Offset = '0x0'; Size = $image.Length; Hash = $actualHash })
if ($useSplitImages) {
    $splitProperty = $manifest.PSObject.Properties['split_images']
    if ($null -eq $splitProperty -or @($splitProperty.Value).Count -ne 3) {
        throw 'Localized/Apps require exactly three split images: bootloader, partition table, and application.'
    }
    $splitDirectory = Get-Item -LiteralPath (Join-Path $imageDirectory 'split')
    if (-not $splitDirectory.PSIsContainer -or
        ($splitDirectory.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
        throw 'The split image directory must be a normal directory inside the package.'
    }
    $offsets = @{ 'split/bootloader.bin' = [long]0; 'split/partition-table.bin' = [long]0x8000; 'split/esp-brookesia.bin' = [long]0x10000 }
    $limits = @{ 'split/bootloader.bin' = [long]0x8000; 'split/partition-table.bin' = [long]0x1000; 'split/esp-brookesia.bin' = $flashBytes - 0x10000 }
    $seen = @{}
    $writeImages = @()
    foreach ($splitRecord in $splitProperty.Value) {
        $splitName = Get-RequiredString $splitRecord 'file' 'split_images entry'
        # A strict package-relative allowlist also rejects traversal, rooted
        # paths, alternate data streams, and unintended NVS/data images.
        if (-not $offsets.ContainsKey($splitName) -or $seen.ContainsKey($splitName)) {
            throw "Invalid or repeated split image filename: $splitName"
        }
        $seen[$splitName] = $true
        $offsetText = Get-RequiredString $splitRecord 'offset' 'split_images entry'
        if ($offsetText -notmatch '^0[xX][0-9a-fA-F]{1,8}$') { throw "Invalid split image offset: $offsetText" }
        $offset = [Convert]::ToInt64($offsetText.Substring(2), 16)
        if ($offset -ne $offsets[$splitName]) { throw "Unexpected offset for $splitName`: $offsetText" }
        $splitSize = Get-RequiredSize $splitRecord 'split_images entry'
        if ($splitSize -gt $limits[$splitName]) { throw "Split image exceeds its allowed flash range: $splitName" }
        $splitHash = Get-RequiredString $splitRecord 'sha256' 'split_images entry'
        if ($splitHash -notmatch '^[0-9a-fA-F]{64}$') { throw "Invalid split image SHA256: $splitName" }
        $splitPath = Join-Path $imageDirectory $splitName
        $splitFile = Get-Item -LiteralPath $splitPath
        if ($splitFile.PSIsContainer -or $splitFile.Length -ne $splitSize -or
            ($splitFile.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
            throw "Split image size/type mismatch: $splitName"
        }
        $verifiedSplitHash = (Get-FileHash -LiteralPath $splitPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($verifiedSplitHash -ne $splitHash.ToLowerInvariant()) { throw "Split image SHA256 mismatch: $splitName. No device accessed." }
        $writeImages += [pscustomobject]@{ Path = $splitFile.FullName; Offset = ('0x{0:x}' -f $offset); Address = $offset; Size = $splitSize; Hash = $verifiedSplitHash }
    }
    $writeImages = @($writeImages | Sort-Object Address)

    # Check the actual incoming partition table, not just its manifest, before
    # claiming the established NVS and PHY locations are preserved. All writes
    # start on 4 KiB boundaries: boot/table end before NVS; app starts after PHY.
    $tableBytes = [System.IO.File]::ReadAllBytes($writeImages[1].Path)
    $partitions = @{}
    for ($position = 0; $position + 32 -le $tableBytes.Length; $position += 32) {
        $magic = [BitConverter]::ToUInt16($tableBytes, $position)
        if ($magic -eq 0xffff -or $magic -eq 0xebeb) { break }
        if ($magic -ne 0x50aa) { throw 'Invalid ESP-IDF partition table entry.' }
        $label = [System.Text.Encoding]::ASCII.GetString($tableBytes, $position + 12, 16).Trim([char]0)
        if ($partitions.ContainsKey($label)) { throw 'Duplicate partition label.' }
        $partitions[$label] = [pscustomobject]@{
            Type = $tableBytes[$position + 2]; Subtype = $tableBytes[$position + 3]
            Offset = [BitConverter]::ToUInt32($tableBytes, $position + 4)
            Size = [BitConverter]::ToUInt32($tableBytes, $position + 8)
        }
    }
    foreach ($required in @('nvs', 'phy_init', 'factory')) {
        if (-not $partitions.ContainsKey($required)) { throw "Missing required partition: $required" }
    }
    $nvs = $partitions['nvs']; $phy = $partitions['phy_init']; $appPartition = $partitions['factory']
    if ($partitions.Count -ne 3 -or
        $nvs.Type -ne 1 -or $nvs.Subtype -ne 2 -or $nvs.Offset -ne 0x9000 -or $nvs.Size -ne 0x6000 -or
        $phy.Type -ne 1 -or $phy.Subtype -ne 1 -or $phy.Offset -ne 0xf000 -or $phy.Size -ne 0x1000 -or
        $appPartition.Type -ne 0 -or $appPartition.Subtype -ne 0 -or $appPartition.Offset -ne 0x10000 -or
        $appPartition.Size -lt $writeImages[2].Size -or ($appPartition.Size % 0x1000) -ne 0 -or
        ([long]$appPartition.Offset + [long]$appPartition.Size) -gt $flashBytes) {
        throw 'Split images do not match the supported NVS/PHY/application partition layout.'
    }
}

$pythonCommand = Get-Command -Name $Python -CommandType Application -ErrorAction Stop
if (@($pythonCommand).Count -ne 1) { throw 'Python must resolve to exactly one executable.' }
$pythonExecutable = $pythonCommand.Source
$backupDirectory = Join-Path $workspace 'backups'
$reuseBackup = $PSBoundParameters.ContainsKey('ExistingBackup')
if ($reuseBackup) {
    if ([string]::IsNullOrWhiteSpace($ExistingBackup)) { throw 'ExistingBackup cannot be blank.' }
    $backup = Get-Item -LiteralPath $ExistingBackup
    if ($backup.PSIsContainer -or $backup.Length -ne $flashBytes) {
        throw 'ExistingBackup must be a complete 16777216-byte flash image. No device accessed.'
    }
    $backupPath = $backup.FullName
    $sidecarPath = "$backupPath.sha256"
    $sidecar = Get-Item -LiteralPath $sidecarPath
    if ($sidecar.PSIsContainer) { throw 'ExistingBackup SHA256 sidecar must be a file.' }
    $sidecarText = (Get-Content -LiteralPath $sidecarPath -Raw).Trim()
    if ($sidecarText -notmatch '^(?<digest>[0-9a-fA-F]{64})(?:[ \t]+[ *]?(?<filename>[^\r\n]+))?$') {
        throw 'Invalid SHA256 sidecar for ExistingBackup. No device accessed.'
    }
    $expectedBackupHash = $Matches['digest'].ToLowerInvariant()
    if ($Matches.ContainsKey('filename') -and
        $Matches['filename'] -ne [System.IO.Path]::GetFileName($backupPath)) {
        throw 'ExistingBackup SHA256 sidecar names a different file. No device accessed.'
    }
    $backupHash = (Get-FileHash -LiteralPath $backupPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($backupHash -ne $expectedBackupHash) {
        throw "ExistingBackup SHA256 mismatch. Expected $expectedBackupHash; got $backupHash. No device accessed."
    }
}
else {
    $timestamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmssfffZ')
    $backupPath = Join-Path $backupDirectory "$timestamp-$Port-$Variant.bin"
    $sidecarPath = "$backupPath.sha256"
    if ((Test-Path -LiteralPath $backupPath) -or (Test-Path -LiteralPath $sidecarPath)) {
        throw 'Backup filename already exists. Retry to obtain a new timestamp; no backup will be overwritten.'
    }
}
$commonArguments = @('-m', 'esptool', '--chip', 'esp32s3', '--port', $Port, '--baud', '460800')
# Keep the backup/write handoff in the loader instead of booting the old app.
# Explicit reset settings also prevent ESPTOOL_BEFORE/AFTER overrides.
# Suppress backspace-only progress lines so redirected PowerShell output cannot
# fill its pipe with one long, unterminated line during a full-flash backup.
$readArguments = $commonArguments + @(
    '--before', 'default_reset', '--after', 'no_reset_stub',
    'read_flash', '--no-progress', '0x0', '0x1000000', $backupPath
)
$writeArguments = $commonArguments + @(
    '--before', $(if ($reuseBackup) { 'default_reset' } else { 'no_reset' }), '--after', 'hard_reset',
    'write_flash', '--flash_freq', 'keep', '--flash_mode', 'keep', '--flash_size', 'keep'
)
foreach ($writeImage in $writeImages) { $writeArguments += @($writeImage.Offset, $writeImage.Path) }

Write-Host "Variant: $Variant"
Write-Host "Performance demo: $($Performance -or $Buffered -or $Corners -or $Localized -or $Apps)"
Write-Host "Buffered experiment: $($Buffered -or $Corners -or $Localized -or $Apps)"
Write-Host "Corner calibration experiment: $Corners"
Write-Host "Localized UI: $($Localized -or $Apps)"
Write-Host "Application pack: $Apps"
Write-Host "Firmware: $imagePath"
Write-Host "Verified SHA256: $actualHash"
if ($useSplitImages) {
    Write-Host 'Write mode: validated split images; NVS/PHY range 0x9000-0xFFFF is not written.'
    foreach ($writeImage in $writeImages) { Write-Host "  $($writeImage.Offset): $($writeImage.Path) ($($writeImage.Size) bytes, SHA256 $($writeImage.Hash))" }
}
else {
    Write-Host 'Write mode: merged image; intervening gaps including NVS are overwritten.'
}
Write-Host "Backup (16 MB): $backupPath"
Write-Host "Backup SHA256 sidecar: $sidecarPath"
if ($reuseBackup) {
    Write-Host "1. Reuse existing 16777216-byte backup; verified SHA256: $backupHash"
    Write-Host '   Skip read_flash; keep the existing backup and sidecar unchanged.'
    Write-Host ('2. After rechecking firmware SHA256: ' + (Format-Command $pythonExecutable $writeArguments))
}
else {
    Write-Host ('1. ' + (Format-Command $pythonExecutable $readArguments))
    Write-Host '   Leave the device in the flasher stub between backup and write.'
    Write-Host '2. Require read success and exactly 16777216 bytes; write the backup SHA256 sidecar.'
    Write-Host ('3. Only after those checks: ' + (Format-Command $pythonExecutable $writeArguments))
}

if ($DryRun) {
    Write-Host 'DRY RUN complete: no Python invocation, serial access, backup, or flash write.'
    return
}

if (-not $reuseBackup) {
    New-Item -ItemType Directory -Path $backupDirectory -Force | Out-Null
    # Atomically reserve a new filename. esptool writes only this invocation's empty
    # placeholder; an existing backup makes CreateNew fail before any serial access.
    $reservation = [System.IO.File]::Open(
        $backupPath, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None
    )
    $reservation.Dispose()

    Invoke-Esptool $readArguments 'Full flash backup'
    $backup = Get-Item -LiteralPath $backupPath
    if ($backup.Length -ne $flashBytes) {
        throw "Backup has $($backup.Length) bytes, expected $flashBytes. Image was not flashed; incomplete backup retained."
    }
    $backupHash = (Get-FileHash -LiteralPath $backupPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $sidecarStream = [System.IO.File]::Open(
        $sidecarPath, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None
    )
    try {
        $sidecarText = "$backupHash  $([System.IO.Path]::GetFileName($backupPath))`n"
        $sidecarBytes = [System.Text.Encoding]::ASCII.GetBytes($sidecarText)
        $sidecarStream.Write($sidecarBytes, 0, $sidecarBytes.Length)
        $sidecarStream.Flush()
    }
    finally {
        $sidecarStream.Dispose()
    }
    Write-Host "Backup verified: $backupHash"
}

# Catch local image changes after validation, including during a new backup.
$preWriteHash = (Get-FileHash -LiteralPath $imagePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($preWriteHash -ne $actualHash) {
    throw 'Firmware changed after validation. Image was not flashed; the verified backup was retained.'
}
foreach ($writeImage in $writeImages) {
    $preWriteImage = Get-Item -LiteralPath $writeImage.Path
    $preWriteImageHash = (Get-FileHash -LiteralPath $writeImage.Path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($preWriteImage.PSIsContainer -or $preWriteImage.Length -ne $writeImage.Size -or $preWriteImageHash -ne $writeImage.Hash) {
        throw 'A flash input changed after validation. Image was not flashed; the verified backup was retained.'
    }
}
Invoke-Esptool $writeArguments 'Firmware write'
Write-Host "Firmware write completed. Backup retained at $backupPath"
