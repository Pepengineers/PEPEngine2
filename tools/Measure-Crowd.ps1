param(
    [string]$Exe = "$PSScriptRoot/../Build/bin/x64/Release/CrowdBenchmark/CrowdBenchmark.exe",
    [Parameter(Mandatory)][string]$Output,
    [ValidateRange(0,2147483647)][int]$People = 10,
    [ValidateRange(0,2147483647)][int]$Foxes = 5,
    [ValidateSet('Original','Off','On')][string]$Instancing = 'On',
    [ValidateRange(1,20)][int]$Runs = 3,
    [ValidateRange(1,100000)][int]$Samples = 600,
    [ValidateRange(0,100000)][int]$Warmup = 120
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$Exe = (Resolve-Path -LiteralPath $Exe).Path
$Output = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Force -Path $Output | Out-Null
if (Test-Path (Join-Path $Output 'run-1.json')) { throw 'Output already contains measurements; use a new directory' }
$fixturePath = Join-Path (Split-Path $Exe -Parent) 'Config/Engine.ini'
$saved = $null
$fixture = (git -C $repo show HEAD:Config/Engine.ini) -join "`n"
if ($LASTEXITCODE) { throw 'Cannot load committed benchmark fixture' }
$settings = [ordered]@{
    'App.NPC.Count' = "$People"; 'App.Fox.Count' = "$Foxes"; 'App.RenderInBackground' = 'true'
    'App.ReflectionProbes.Count' = '0'; 'App.Grass.Count' = '0'; 'App.Water.Count' = '0'; 'App.Particles.Count' = '0'
    'App.ReflectionProbes.ShowSpheres' = 'false'; 'App.Bounds.Show' = 'false'
    'Render.Skinning.Enabled' = 'true'; 'Render.Skinning.Quality' = '3'
    'Render.Upscaler.Provider' = '0'; 'Render.FrameGeneration.Provider' = '0'; 'Render.AO.Enabled' = '0'
    'Render.SSR.Enabled' = 'false'; 'Render.Reflection.Enabled' = 'false'; 'Render.MotionBlur.Enabled' = 'false'
    'Render.DOF.Enabled' = 'false'; 'Render.Particles.Enabled' = 'false'; 'Render.Grass.Enabled' = 'false'
    'Render.Fluid.Enabled' = 'false'; 'Render.NumFrames' = '3'; 'Jobs.ThreadCount' = '4'
}
if ($Instancing -ne 'Original') { $settings['Render.Skinning.Instancing'] = ($Instancing -eq 'On').ToString().ToLowerInvariant() }
foreach ($entry in $settings.GetEnumerator()) {
    $pattern = '(?mi)^' + [regex]::Escape($entry.Key) + '\s*=.*$'
    $line = $entry.Key + ' = ' + $entry.Value
    if ([regex]::IsMatch($fixture, $pattern)) { $fixture = [regex]::Replace($fixture, $pattern, $line) }
    else { $fixture = $fixture.Replace('[ConsoleVariables]', "[ConsoleVariables]`n$line") }
}
$gpuMutex = [Threading.Mutex]::new($false, 'Local\PEPEngine2FeatureGpuValidation')
$acquired = $false
try {
    try { $acquired = $gpuMutex.WaitOne(1000) } catch [Threading.AbandonedMutexException] { $acquired = $true }
    if (-not $acquired) { throw 'GPU validation mutex is busy' }
    $saved = [IO.File]::ReadAllBytes($fixturePath)
    $manifest = [ordered]@{
        revision = (git -C $repo rev-parse HEAD); executable_sha256 = (Get-FileHash -LiteralPath $Exe).Hash
        people = $People; foxes = $Foxes; instancing = $Instancing; runs = $Runs; samples = $Samples; warmup = $Warmup
        settings = $settings; cpu = $env:PROCESSOR_IDENTIFIER; gpu_before = @(& nvidia-smi --query-gpu=name,driver_version,memory.total,clocks.gr,clocks.mem,temperature.gpu,power.draw --format=csv,noheader,nounits)
    }
    [IO.File]::WriteAllText((Join-Path $Output 'fixture.ini'), $fixture)
    $first = $null
    for ($run = 1; $run -le $Runs; ++$run) {
        [IO.File]::WriteAllText($fixturePath, $fixture)
        $reportPath = Join-Path $Output "run-$run.json"
        & $Exe $reportPath $Samples $Warmup 2>&1 | Tee-Object -FilePath (Join-Path $Output "run-$run.log")
        if ($LASTEXITCODE) { throw "Crowd benchmark failed with exit $LASTEXITCODE" }
        $result = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
        if ($result.humans -ne $People -or $result.foxes -ne $Foxes) { throw 'Benchmark population does not match request' }
        if ($null -ne $first -and ($result.spawn_checksum -ne $first.spawn_checksum -or $result.warm_checksum -ne $first.warm_checksum -or $result.sample_checksum -ne $first.sample_checksum)) { throw 'Crowd state is not deterministic between repetitions' }
        $first = $result
    }
    $manifest['gpu_after'] = @(& nvidia-smi --query-gpu=name,driver_version,memory.total,clocks.gr,clocks.mem,temperature.gpu,power.draw --format=csv,noheader,nounits)
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Output 'manifest.json') -Encoding utf8
} finally {
    try { if ($null -ne $saved) { [IO.File]::WriteAllBytes($fixturePath, $saved) } }
    finally { if ($acquired) { $gpuMutex.ReleaseMutex() }; $gpuMutex.Dispose() }
}
