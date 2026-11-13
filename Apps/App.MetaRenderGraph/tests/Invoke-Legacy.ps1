param(
    [ValidateSet('BaseRenderPipelineTests','RenderGraphTests')][string]$Target = 'BaseRenderPipelineTests',
    [switch]$Custom
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../../..").Path
$output = Join-Path $repo "Build/bin/x64/Debug/$Target"
$evidence = Join-Path $repo 'Build/Verification/meta-render-graph-u2'
$label = if ($Custom) { "$Target-custom" } else { "$Target-default" }
# Test output only; production config is never edited.
New-Item -ItemType Directory -Force "$output/Config" | Out-Null
Copy-Item "$PSScriptRoot/../Configs/Startup.ini" "$output/Config/Engine.ini"
$start = @{FilePath="$output/$Target.exe"; WorkingDirectory=$repo; WindowStyle='Hidden'; PassThru=$true;
    RedirectStandardOutput="$evidence/$label-stdout.log"; RedirectStandardError="$evidence/$label-stderr.log"}
if ($Custom) { $start.ArgumentList = @('--custom') }
$process = Start-Process @start
if (-not $process.WaitForExit(60000)) { Stop-Process -Id $process.Id; throw 'Legacy test exceeded 60 seconds' }
Get-Content "$evidence/$label-stdout.log" -Tail 3
if ($process.ExitCode -ne 0) { Get-Content "$evidence/$label-stderr.log" -Tail 5; throw "Legacy test exit=$($process.ExitCode)" }
Write-Output "PASS legacy $label exit=0 (60s limit)"
