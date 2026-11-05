param(
    [Parameter(Mandatory)][string]$Destination,
    [ValidateSet('x64', 'Win32')][string]$Architecture = 'x64'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$version = '1.619.6'
$dependencies = Join-Path $repo 'Build/Dependencies'
$archive = Join-Path $dependencies "agility-$version.zip"
$sdk = Join-Path $dependencies "AgilitySDK-$version"
$binaries = Join-Path $sdk "build/native/bin/$($Architecture.ToLowerInvariant())"
$runtime = @('D3D12Core.dll', 'd3d12SDKLayers.dll', 'D3D12StateObjectCompiler.dll')
# Several application projects can request the same cache in a parallel build.
$mutex = [Threading.Mutex]::new($false, 'Local\PEPEngine2.AgilitySDK.1.619.6')
$locked = $false
try {
    $locked = $mutex.WaitOne(60000)
    if (!$locked) { throw 'Timed out waiting for Agility SDK preparation' }
    $required = @($runtime | ForEach-Object { Join-Path $binaries $_ }) + "$sdk/LICENSE.txt"
    if (@($required | Where-Object { !(Test-Path -LiteralPath $_) }).Count) {
        New-Item -ItemType Directory -Force $dependencies | Out-Null
        if (!(Test-Path $archive)) {
            & curl.exe -L --fail --retry 2 "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/$version/microsoft.direct3d.d3d12.$version.nupkg" -o $archive
            if ($LASTEXITCODE) { throw 'Agility SDK download failed' }
        }
        $stream = [IO.File]::OpenRead($archive)
        try { $hash = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($stream)).Replace('-', '') }
        finally { $stream.Dispose() }
        if ($hash -ne
            '08F0489281401AA430FC37322D6C3FC98A8025175AACD714C10D562F4963F1E9') {
            throw 'Agility SDK archive SHA256 mismatch'
        }
        New-Item -ItemType Directory -Force $sdk | Out-Null
        & tar.exe -xf $archive -C $sdk
        if ($LASTEXITCODE) { throw 'Agility SDK extraction failed' }
    }
    New-Item -ItemType Directory -Force $Destination | Out-Null
    foreach ($file in $runtime) {
        Copy-Item -LiteralPath (Join-Path $binaries $file) -Destination $Destination -Force
    }
    Copy-Item -LiteralPath "$sdk/LICENSE.txt" -Destination $Destination -Force
}
finally {
    if ($locked) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
