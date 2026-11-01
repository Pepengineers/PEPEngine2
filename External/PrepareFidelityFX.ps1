param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$pin = 'c6efa6bf7f2027b3ec94f28578bb5965eabb9e55'
$dependencies = Join-Path $repo 'Build/dependencies'
$archive = Join-Path $dependencies 'fidelityfx-sdk.tar.gz'
$source = Join-Path $dependencies "FidelityFX-SDK-$pin"
$build = Join-Path $repo 'Build/FidelityFX'
New-Item -ItemType Directory -Force $dependencies | Out-Null
if (!(Test-Path "$source/sdk/CMakeLists.txt")) {
    if (!(Test-Path $archive)) {
        & curl.exe -L --fail --show-error "https://codeload.github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tar.gz/$pin" -o $archive
        if ($LASTEXITCODE) { throw 'FidelityFX download failed' }
    }
    $hash = (Get-FileHash $archive -Algorithm SHA256).Hash
    if ($hash -ne 'EFF7E8E371B2D461B980F8C75583133F771E940ED7AA00779DBBAF6DD2F88746') {
        throw 'FidelityFX archive checksum mismatch'
    }
    & tar -xzf $archive -C $dependencies "FidelityFX-SDK-$pin/sdk" "FidelityFX-SDK-$pin/LICENSE.txt"
    if ($LASTEXITCODE) { throw 'FidelityFX extraction failed' }
}
$cmake = 'C:/Program Files/Microsoft Visual Studio/18/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
if (!(Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }
if (!(Test-Path "$build/CMakeCache.txt")) {
    & $cmake -S "$source/sdk" -B $build -G 'Visual Studio 18 2026' -A x64 -DFFX_API_BACKEND=DX12_X64 -DFFX_DOF=ON -DFFX_ALL=OFF
    if ($LASTEXITCODE) { throw 'FidelityFX configure failed' }
}
& $cmake --build $build --config $Configuration --target ffx_dof_x64 ffx_backend_dx12_x64 -- /m:1 /nr:false /p:PreferredToolArchitecture=x64
if ($LASTEXITCODE) { throw 'FidelityFX build failed' }
