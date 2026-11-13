param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [ValidateSet('App.MetaRenderGraph', 'ApplicationSmokeTests', 'DirectExecutionGpuTests')][string]$Target = 'ApplicationSmokeTests'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../../..").Path
$output = Join-Path $repo "Build/bin/x64/$Configuration/$Target"
$evidence = Join-Path $repo "Build/Verification/meta-render-graph-u2/$Configuration-$Target"
New-Item -ItemType Directory -Force $evidence | Out-Null
$protected = @("$repo/Config/Engine.ini", "$repo/Build/bin/x64/$Configuration/App/Config/Engine.ini")
$before = @{}
foreach ($path in $protected) { $before[$path] = if (Test-Path $path) { (Get-FileHash $path).Hash } else { 'absent' } }
# Only the experiment's generated INI is poisoned; production configuration is read-only here.
New-Item -ItemType Directory -Force "$output/Config" | Out-Null
@'
[ConsoleVariables]
Render.PrimaryAdapter = 999
Render.Particles.SecondaryAdapter = 999
Render.Fluid.SecondaryAdapter = 999
Render.Particles.AsyncCompute = true
'@ | Set-Content "$output/Config/Engine.ini"
$log = "$output/Logs/Engine.log"
$previous = if (Test-Path $log) { (Get-Content $log -Raw).Length } else { 0 }
$arguments = if ($Target -eq 'App.MetaRenderGraph') { @('--smoke') } else { @() }
$start = @{FilePath = "$output/$Target.exe"; WorkingDirectory = $repo; WindowStyle = 'Hidden'; PassThru = $true}
if ($arguments.Count) { $start.ArgumentList = $arguments }
$process = Start-Process @start
if (-not $process.WaitForExit(60000)) { Stop-Process -Id $process.Id; throw 'GPU smoke exceeded 60 seconds' }
if ($process.ExitCode -ne 0) { throw "GPU smoke exit code $($process.ExitCode); see $log" }
$text = (Get-Content $log -Raw).Substring($previous)
$text | Set-Content "$evidence/runtime.log"
if (($text | Select-String 'GPU adapter:' -AllMatches).Matches.Count -ne 1) { throw 'Expected exactly one initialized GDX12Device in runtime log' }
if ($text -match '\[Particles\].*Offload|\[Water\].*Offload') { throw 'Unexpected offload runtime' }
if ($text -match '\[ERROR\]|GPU retirement failed') { throw 'Runtime reported an error' }
if ($text -notmatch 'Runtime legacy graph Execute calls=0') { throw 'Legacy execution counter did not remain zero' }
if ($text -notmatch 'PASS DirectExecutionGpuTests:' -or $text -notmatch 'U2 native DIRECT executor selected') { throw 'Missing U2 execution evidence' }
if ($text -notmatch 'Smoke passed: 8 presented frames' -or $text -notmatch 'Smoke shutdown complete') { throw 'Missing runtime success markers' }
if ((Get-FileHash "$output/Config/Engine.ini").Hash -ne (Get-FileHash "$output/Configs/Startup.ini").Hash) { throw 'Startup INI was not restored' }
foreach ($path in $protected) {
    $after = if (Test-Path $path) { (Get-FileHash $path).Hash } else { 'absent' }
    if ($before[$path] -ne $after) { throw "Production INI changed: $path" }
}
Copy-Item "$output/Logs/direct-schedule.txt" "$evidence/direct-schedule.txt"
Copy-Item "$output/Logs/reference.dot" "$evidence/reference.dot"
Copy-Item "$output/Config/Engine.ini" "$evidence/effective.ini"
$before | ConvertTo-Json | Set-Content "$evidence/protected-config-hashes.json"
Write-Output "PASS $Configuration ${Target}: exit=0; device initializations=1; poison ignored; production INIs unchanged; 8 frames/resize/shutdown; D3D12 errors=0"
