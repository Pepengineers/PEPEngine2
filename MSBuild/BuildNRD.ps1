param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Output,
    [Parameter(Mandatory)][ValidateSet('Debug', 'Release')][string]$Configuration,
    [Parameter(Mandatory)][string]$VSInstallDir,
    [Parameter(Mandatory)][string]$VCToolsVersion
)
$ErrorActionPreference = 'Stop'
$Source = (Resolve-Path -LiteralPath $Source).Path
$Output = [IO.Path]::GetFullPath($Output)
Import-Module (Join-Path $VSInstallDir 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $VSInstallDir -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64 -vcvars_ver=$VCToolsVersion"
$tools = Join-Path $VSInstallDir 'Common7/IDE/CommonExtensions/Microsoft/CMake'
$cmake = Join-Path $tools 'CMake/bin/cmake.exe'
$ninja = Join-Path $tools 'Ninja/ninja.exe'
if (!(Test-Path -LiteralPath $cmake) -or !(Test-Path -LiteralPath $ninja)) {
    throw 'NRD requires the Visual Studio C++ CMake tools for Windows component (CMake 3.30 or newer).'
}
$compiler = Join-Path $env:VCToolsInstallDir 'bin/Hostx64/x64/cl.exe'
$ErrorActionPreference = 'Continue'
& $cmake -S $Source -B $Output -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_CXX_COMPILER=$compiler" `
    "-DCMAKE_BUILD_TYPE=$Configuration" "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$Output/lib" `
    -DNRD_STATIC_LIBRARY=ON -DNRD_NRI=OFF -DSHADERMAKE_TOOL=OFF `
    -DNRD_EMBEDS_DXIL_SHADERS=OFF -DNRD_EMBEDS_DXBC_SHADERS=OFF -DNRD_EMBEDS_SPIRV_SHADERS=OFF `
    -DNRD_NORMAL_ENCODING=4 -DNRD_ROUGHNESS_ENCODING=1
if ($LASTEXITCODE) { exit $LASTEXITCODE }
& $cmake --build $Output --target NRD --parallel 4
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$ErrorActionPreference = 'Stop'
$shaders = Join-Path $Output 'Shaders'
$mathLib = Join-Path $Output '_deps/mathlib-src'
New-Item -ItemType Directory -Force $shaders | Out-Null
Copy-Item -Path (Join-Path $Source 'Shaders/*') -Destination $shaders -Recurse -Force
Copy-Item -LiteralPath (Join-Path $mathLib 'ml.hlsli'), (Join-Path $mathLib 'Guts') -Destination $shaders -Recurse -Force
Copy-Item -LiteralPath (Join-Path $mathLib 'LICENSE.txt') -Destination (Join-Path $Output 'MathLib.LICENSE.txt') -Force
New-Item -ItemType File -Force (Join-Path $Output "$VCToolsVersion.stamp") | Out-Null
foreach ($file in @('lib/NRD.lib', 'Shaders/NRD.hlsli', 'Shaders/NRDConfig.hlsli', 'Shaders/ml.hlsli')) {
    (Get-Item -LiteralPath (Join-Path $Output $file)).LastWriteTime = Get-Date
}
(Get-Item -LiteralPath (Join-Path $Source 'Shaders/NRDConfig.hlsli')).LastWriteTime = Get-Date
