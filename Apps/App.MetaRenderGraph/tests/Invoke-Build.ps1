param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [ValidateSet('PlanTests','DirectExecutionGpuTests','App.MetaRenderGraph','ApplicationSmokeTests','App','BaseRenderPipelineTests','RenderGraphTests')][string]$Target = 'App.MetaRenderGraph'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../../..").Path
$evidence = Join-Path $repo 'Build/Verification/meta-render-graph-u2'
New-Item -ItemType Directory -Force $evidence | Out-Null
$folder = if ($Target -in @('App','BaseRenderPipelineTests','RenderGraphTests')) { 'App' } else { 'App.MetaRenderGraph' }
$log = Join-Path $evidence "$Target-$Configuration-build.log"
$stdout = Join-Path $evidence "$Target-$Configuration-stdout.log"
$stderr = Join-Path $evidence "$Target-$Configuration-stderr.log"
$build = 'C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe'
$arguments = @("Apps/$folder/$Target.vcxproj", "/p:Configuration=$Configuration", '/p:Platform=x64', '/m:2', '/nr:false', '/v:quiet', '/fl', "/flp:logfile=$log")
$process = Start-Process -FilePath $build -ArgumentList $arguments -WorkingDirectory $repo -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
if (-not $process.WaitForExit(600000)) {
    & taskkill /PID $process.Id /T /F | Out-Null
    throw "Build exceeded 600 seconds: $Target $Configuration"
}
if ($process.ExitCode -ne 0) {
    Get-Content $stdout -Tail 12
    throw "Build failed: $Target $Configuration exit=$($process.ExitCode); $log"
}
Write-Output "PASS build $Target $Configuration (600s limit): $log"
