param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$pin = 'c6efa6bf7f2027b3ec94f28578bb5965eabb9e55'
$dependencies = Join-Path $repo 'Build/Dependencies'
$archive = Join-Path $dependencies 'fidelityfx.tar.gz'
$sdk = Join-Path $dependencies "FidelityFX-SDK-$pin/sdk"
$build = Join-Path $repo 'Build/FidelityFX'
New-Item -ItemType Directory -Force $dependencies | Out-Null
if (!(Test-Path "$sdk/CMakeLists.txt")) {
    if (!(Test-Path $archive)) {
        & curl.exe -L --fail --retry 2 "https://codeload.github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tar.gz/$pin" -o $archive
        if ($LASTEXITCODE) { throw 'FidelityFX download failed' }
    }
    $archiveStream = [System.IO.File]::OpenRead($archive)
    try { $archiveHash = [BitConverter]::ToString([System.Security.Cryptography.SHA256]::Create().ComputeHash($archiveStream)).Replace('-', '') }
    finally { $archiveStream.Dispose() }
    if ($archiveHash -ne 'EFF7E8E371B2D461B980F8C75583133F771E940ED7AA00779DBBAF6DD2F88746') {
        throw 'FidelityFX archive SHA256 mismatch'
    }
    & tar.exe -xf $archive -C $dependencies "FidelityFX-SDK-$pin/sdk" "FidelityFX-SDK-$pin/LICENSE.txt"
    if ($LASTEXITCODE) { throw 'FidelityFX extraction failed' }
}
foreach ($patchName in @('cold-history.patch', 'probe-fallback.patch')) {
    $patch = Join-Path $repo "External/FidelityFX/$patchName"
    $ErrorActionPreference = 'Continue'
    & git -C $sdk apply --reverse --check $patch 2>&1 | Out-Null
    $patchStatus = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($patchStatus -ne 0) {
        & git -C $sdk apply --check $patch
        if ($LASTEXITCODE) { throw "FidelityFX $patchName does not match the pinned source" }
        & git -C $sdk apply $patch
        if ($LASTEXITCODE) { throw "FidelityFX $patchName failed" }
    }
}
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
& $cmake -S $sdk -B $build -G 'Visual Studio 18 2026' -A x64 -DFFX_API_BACKEND=DX12_X64 -DFFX_ALL=OFF -DFFX_SSSR=ON -DFFX_DENOISER=ON -DFFX_DOF=ON '-DCMAKE_CXX_FLAGS_RELEASE=/MD /O2 /DNDEBUG /Z7 /GL-'
if ($LASTEXITCODE) { throw 'FidelityFX CMake configuration failed' }
& $cmake --build $build --config $Configuration --target ffx_backend_dx12_x64 ffx_sssr_x64 ffx_denoiser_x64 ffx_dof_x64 -- /m:1 /nr:false /p:PreferredToolArchitecture=x64
if ($LASTEXITCODE) { throw 'FidelityFX build failed' }
