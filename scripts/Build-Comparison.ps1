param(
    [ValidateSet(40, 80)][int[]]$MHz = @(40, 80),
    [switch]$Performance,
    [switch]$Buffered,
    [switch]$Corners,
    [switch]$Localized,
    [switch]$Apps,
    [switch]$SeedSamples,
    [string]$BuildRoot = 'C:\esp\hosino-qspi',
    [string]$IdfPath = 'C:\esp\esp-idf-v5.5.5'
)
$ErrorActionPreference = 'Stop'
if ($SeedSamples -and -not $Apps) { throw '-SeedSamples is only available with -Apps.' }
if ($Apps) { $Localized = $true }
if ($Corners -and $Localized) { throw '-Corners and -Localized are mutually exclusive.' }
if ($Corners -or $Localized) { $Buffered = $true }
$workspace = Split-Path -Parent $PSScriptRoot
$sourceLock = Get-Content -LiteralPath (Join-Path $workspace 'source-lock.json') -Raw | ConvertFrom-Json
$idfCommit = (& git -C $IdfPath rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $idfCommit -ne $sourceLock.idf_commit) { throw 'ESP-IDF commit does not match source-lock.json' }
$env:IDF_TOOLS_PATH = Join-Path $BuildRoot 'tools'
$env:IDF_TARGET = 'esp32s3'
$env:IDF_CCACHE_ENABLE = '1'
$env:PYTHONIOENCODING = 'utf-8'
. (Join-Path $IdfPath 'export.ps1')
if (-not $env:IDF_PYTHON_ENV_PATH) { throw 'ESP-IDF Python environment is not active' }
$idfPython = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
$stageArgs = @((Join-Path $PSScriptRoot 'prepare_project.py'), '--build-root', $BuildRoot)
if ($Apps) { $stageArgs += '--apps' }
elseif ($Localized) { $stageArgs += '--localized' }
elseif ($Corners) { $stageArgs += '--corners' }
elseif ($Buffered) { $stageArgs += '--buffered' }
elseif ($Performance) { $stageArgs += '--performance' }
& $idfPython @stageArgs
if ($LASTEXITCODE -ne 0) { throw 'Project staging failed' }
$projectName = if ($Apps) { 'project-apps' } elseif ($Localized) { 'project-localized' } elseif ($Corners) { 'project-corners' } elseif ($Buffered) { 'project-buffered' } elseif ($Performance) { 'project-perf' } else { 'project' }
$project = Join-Path $BuildRoot $projectName
$logs = Join-Path $workspace 'evidence'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
foreach ($clock in $MHz) {
    $buildLabel = if ($Apps) { "apps-$clock" } elseif ($Localized) { "localized-$clock" } elseif ($Corners) { "corners-$clock" } elseif ($Buffered) { "buffered-$clock" } elseif ($Performance) { "perf-$clock" } else { "$clock" }
    $build = Join-Path $BuildRoot "build-$buildLabel"
    $configName = if ($Apps) { "sdkconfig.apps.$clock" } elseif ($Localized) { "sdkconfig.localized.$clock" } elseif ($Corners) { "sdkconfig.corners.$clock" } elseif ($Buffered) { "sdkconfig.buffered.$clock" } elseif ($Performance) { "sdkconfig.perf.$clock" } else { "sdkconfig.$clock" }
    $sdkconfig = Join-Path $BuildRoot $configName
    $defaults = Join-Path $project "sdkconfig.$clock.defaults"
    $projectVersion = if ($Apps) { 'qspi-apps-23' } elseif ($Localized) { 'qspi-ui-2' } elseif ($Corners) { 'qspi-corner-1' } elseif ($Buffered) { 'qspi-buf-4' } elseif ($Performance) { 'qspi-perf-1' } else { 'qspi-ab-1' }
    $idfArgs = @((Join-Path $IdfPath 'tools\idf.py'), '-C', $project, '-B', $build,
        '-D', "SDKCONFIG=$sdkconfig", '-D', "SDKCONFIG_DEFAULTS=$defaults",
        '-D', "PROJECT_VER=$projectVersion")
    if ($Apps) {
        # Always write ON/OFF so a previous seed build cannot leak into release.
        $sampleInstall = if ($SeedSamples) { 'ON' } else { 'OFF' }
        $idfArgs += @('-D', "HOSINO_INSTALL_MEDIA_SAMPLES=$sampleInstall")
    }
    $idfArgs += 'build'
    & $idfPython @idfArgs 2>&1 | Tee-Object -FilePath (Join-Path $logs "build-$buildLabel.log")
    if ($LASTEXITCODE -ne 0) { throw "Build failed for $clock MHz" }
    $configDirectory = Join-Path $workspace 'config'
    New-Item -ItemType Directory -Force -Path $configDirectory | Out-Null
    $lockName = if ($Apps) { 'dependencies-apps.lock' } elseif ($Localized) { 'dependencies-localized.lock' } elseif ($Corners) { 'dependencies-corners.lock' } elseif ($Buffered) { 'dependencies-buffered.lock' } elseif ($Performance) { 'dependencies-perf.lock' } else { 'dependencies.lock' }
    Copy-Item -LiteralPath (Join-Path $project 'dependencies.lock') -Destination (Join-Path $configDirectory $lockName)
    Copy-Item -LiteralPath (Join-Path $project 'dependencies.lock') -Destination (Join-Path $build 'dependencies.lock')
}
