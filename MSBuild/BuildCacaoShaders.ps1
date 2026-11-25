param([string]$Dxc, [string]$Source, [string]$Output)
$ErrorActionPreference = 'Stop'
$Dxc = Join-Path ($Dxc.Split(';')[0]) 'dxc.exe'
New-Item -ItemType Directory -Force "$Output/PrecompiledShadersDXIL" | Out-Null
$entries = Select-String -Path "$Source/build_shaders_dxil.bat" -Pattern '-Fh (\S+)\s+-Vn (\S+)\s+-E (\S+)'
if ($entries.Count -ne 33) { throw 'Unexpected CACAO shader manifest' }
foreach ($entry in $entries) {
    $match = $entry.Matches[0]
    & $Dxc -T cs_6_2 -O3 -I $Source -Fh "$Output/$($match.Groups[1].Value)" -Vn $match.Groups[2].Value -E $match.Groups[3].Value "$Source/ffx_cacao.hlsl"
    if ($LASTEXITCODE) { throw "CACAO shader compilation failed: $entry" }
}
