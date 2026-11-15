# Run from an x64 Visual Studio Developer PowerShell after a Debug x64 build.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$output = Join-Path $root 'Build/AssetCatalogTests'
New-Item -ItemType Directory -Path $output -Force | Out-Null

Push-Location $root
try {
    & cl.exe /nologo /std:c++17 /EHsc /MDd /Zi /IApps/App.Base/Include `
        /IExternal/External.Libraries/rapidyaml/src /IExternal/External.Libraries/rapidyaml/ext/c4core/src `
        "/Fo$output/AssetCatalogTests.obj" "/Fd$output/AssetCatalogTests.pdb" "/Fe$output/AssetCatalogTests.exe" `
        Tests/AssetCatalogTests.cpp Build/bin/x64/Debug/App.Base/App.Base.lib `
        Build/bin/x64/Debug/External.Libraries/External.Libraries.lib
    if ($LASTEXITCODE -ne 0) { throw 'Asset catalog test build failed.' }

    & "$output/AssetCatalogTests.exe" "$output/$([Guid]::NewGuid())"
    if ($LASTEXITCODE -ne 0) { throw 'Asset catalog tests failed.' }
    Write-Output 'Asset catalog tests passed.'
} finally {
    Pop-Location
}
