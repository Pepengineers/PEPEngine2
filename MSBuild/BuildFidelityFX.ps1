param(
    [Parameter(Mandatory)][ValidateSet('Debug', 'Release')][string]$Configuration,
    [Parameter(Mandatory)][string]$VSInstallDir,
    [Parameter(Mandatory)][string]$VCToolsVersion,
    [string]$PlatformToolset = 'v143'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$pin = 'c6efa6bf7f2027b3ec94f28578bb5965eabb9e55'
$dependencies = Join-Path $repo 'Build/Dependencies'
$archive = Join-Path $dependencies 'fidelityfx.tar.gz'
$source = Join-Path $dependencies "FidelityFX-SSSR-$pin"
$sdk = Join-Path $source 'sdk'
$build = Join-Path $repo 'Build/External/FidelityFXSSSR'
New-Item -ItemType Directory -Force $dependencies | Out-Null
if (!(Test-Path -LiteralPath "$sdk/CMakeLists.txt")) {
    if (!(Test-Path -LiteralPath $archive)) {
        & curl.exe -L --fail --retry 2 "https://codeload.github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tar.gz/$pin" -o $archive
        if ($LASTEXITCODE) { throw 'FidelityFX download failed.' }
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne 'EFF7E8E371B2D461B980F8C75583133F771E940ED7AA00779DBBAF6DD2F88746') {
        throw 'FidelityFX archive SHA256 mismatch.'
    }
    New-Item -ItemType Directory -Force $source | Out-Null
    & tar.exe -xf $archive --strip-components=1 -C $source "FidelityFX-SDK-$pin/sdk" "FidelityFX-SDK-$pin/LICENSE.txt"
    if ($LASTEXITCODE) { throw 'FidelityFX extraction failed.' }
}
foreach ($name in @('cold-history.patch', 'sssr-spd-counter.patch', 'environment-fallback.patch', 'depth-pyramid.patch')) {
    $patch = Join-Path $repo "External/External.Libraries/FidelityFX/$name"
    $ErrorActionPreference = 'Continue'
    & git -C $sdk apply --ignore-space-change --reverse --check $patch 2>&1 | Out-Null
    $patchStatus = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($patchStatus -ne 0) {
        & git -C $sdk apply --ignore-space-change --check $patch
        if ($LASTEXITCODE) { throw "FidelityFX $name does not match the pinned source." }
        & git -C $sdk apply --ignore-space-change $patch
        if ($LASTEXITCODE) { throw "FidelityFX $name failed." }
    }
}
$cmake = Join-Path $VSInstallDir 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
Import-Module (Join-Path $VSInstallDir 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $VSInstallDir -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64 -vcvars_ver=$VCToolsVersion"
$generator = if ([version]$env:VisualStudioVersion -ge [version]'18.0') { 'Visual Studio 18 2026' } else { 'Visual Studio 17 2022' }
$ErrorActionPreference = 'Continue'
& $cmake -S $sdk -B $build -G $generator -A x64 -T "$PlatformToolset,version=$VCToolsVersion" `
    -DFFX_API_BACKEND=DX12_X64 -DFFX_ALL=OFF -DFFX_SSSR=ON -DFFX_DENOISER=ON `
    '-DCMAKE_CXX_FLAGS_RELEASE=/MD /O2 /DNDEBUG /Z7 /GL-' '-DCMAKE_CXX_FLAGS_DEBUG=/MDd /Od /D_DEBUG /Z7 /GL-'
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $cmake --build $build --config $Configuration --target ffx_backend_dx12_x64 ffx_sssr_x64 ffx_denoiser_x64 -- /m:2 /nr:false /p:PreferredToolArchitecture=x64
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$postfix = if ($Configuration -eq 'Debug') { 'd' } else { '' }
foreach ($name in @('ffx_backend_dx12_x64', 'ffx_sssr_x64', 'ffx_denoiser_x64')) {
    (Get-Item -LiteralPath (Join-Path $sdk "bin/ffx_sdk/$name$postfix.lib")).LastWriteTime = Get-Date
}
New-Item -ItemType File -Force (Join-Path $build "$VCToolsVersion-$Configuration.stamp") | Out-Null
