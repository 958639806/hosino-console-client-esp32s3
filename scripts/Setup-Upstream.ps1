#requires -Version 5.1
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
$lock = Get-Content -LiteralPath (Join-Path $workspace 'source-lock.json') -Raw | ConvertFrom-Json
$target = Join-Path $workspace 'upstream\waveshare-esp32-s3-touch-amoled-2.16'
if (-not (Test-Path -LiteralPath $target)) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    & git -c core.longpaths=true clone --no-checkout $lock.upstream_repository $target
    if ($LASTEXITCODE -ne 0) { throw 'Upstream clone failed.' }
    & git -C $target config core.longpaths true
    if ($LASTEXITCODE -ne 0) { throw 'Setting local Git long-path support failed.' }
    & git -C $target checkout --detach $lock.upstream_commit
    if ($LASTEXITCODE -ne 0) { throw 'Pinned upstream checkout failed.' }
}
$head = (& git -C $target rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $head -ne $lock.upstream_commit) {
    throw 'Existing upstream is at a different commit. Move it aside or select the pinned commit manually.'
}
$dirty = & git -C $target status --porcelain
if ($LASTEXITCODE -ne 0 -or $dirty) { throw 'Upstream must be a clean Git checkout.' }
Write-Host "Pinned upstream ready: $head"

