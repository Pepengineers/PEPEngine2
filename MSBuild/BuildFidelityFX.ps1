param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$CMake
)
$ErrorActionPreference = 'Stop'
$repoPath = Split-Path -Parent $PSScriptRoot
$sourcePath = Join-Path $repoPath 'Build\FidelityFXSource'
$buildPath = Join-Path $repoPath 'Build\FidelityFXBuild'
$revision = 'c6efa6bf7f2027b3ec94f28578bb5965eabb9e55'

if (-not (Test-Path -LiteralPath $sourcePath)) {
    & git clone --depth 1 --branch v1.1.4 --filter=blob:none --sparse https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git $sourcePath
    if ($LASTEXITCODE -ne 0) { throw 'Cannot fetch FidelityFX SDK v1.1.4' }
}
$actualRevision = & git -C $sourcePath rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $actualRevision -ne $revision) {
    throw "Expected FidelityFX $revision at $sourcePath; found $actualRevision"
}
if (-not (Test-Path -LiteralPath (Join-Path $sourcePath 'sdk\tools\binary_store\FidelityFX_SC.exe'))) {
    & git -C $sourcePath sparse-checkout set sdk/include sdk/src sdk/libs sdk/tools/binary_store
    if ($LASTEXITCODE -ne 0) { throw 'Cannot restore FidelityFX source and shader compiler' }
}
# Apply local adaptations to the pinned SDK before configuring it.
foreach ($relativePatch in @('FidelityFX-cmake.patch', 'Patches\FidelityFX-probe-fallback.patch')) {
    $patchPath = Join-Path $PSScriptRoot $relativePatch
    & git -C $sourcePath apply --reverse --check --quiet $patchPath
    if ($LASTEXITCODE -ne 0) {
        & git -C $sourcePath apply $patchPath
        if ($LASTEXITCODE -ne 0) { throw "Cannot apply FidelityFX adaptation: $relativePatch" }
    }
}
if (-not $CMake) {
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($command) { $CMake = $command.Source }
    else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        $CMake = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    }
}
if (-not (Test-Path -LiteralPath $CMake)) { throw 'Visual Studio CMake is required to build FidelityFX' }
& $CMake -S (Join-Path $sourcePath 'sdk') -B $buildPath -G 'Visual Studio 18 2026' -A x64 `
    -DFFX_API_BACKEND=DX12_X64 -DFFX_API_DX12=ON -DFFX_ALL=OFF `
    -DFFX_DOF=ON -DFFX_SSSR=ON -DFFX_DENOISER=ON -DFFX_AUTO_COMPILE_SHADERS=ON
if ($LASTEXITCODE -ne 0) { throw 'FidelityFX CMake configuration failed' }
$targets = @('ffx_dof_x64', 'ffx_sssr_x64', 'ffx_denoiser_x64', 'ffx_backend_dx12_x64')
& $CMake --build $buildPath --config $Configuration --parallel 4 --target $targets
if ($LASTEXITCODE -ne 0) { throw 'FidelityFX compilation failed' }
$suffix = if ($Configuration -eq 'Debug') { 'd' } else { '' }
foreach ($target in $targets) {
    $library = Join-Path $sourcePath "sdk\bin\ffx_sdk\$target$suffix.lib"
    if (-not (Test-Path -LiteralPath $library) -or (Get-Item -LiteralPath $library).Length -eq 0) {
        throw "FidelityFX build did not produce $library"
    }
}
