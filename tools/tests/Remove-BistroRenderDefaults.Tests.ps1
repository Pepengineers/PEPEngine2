$ErrorActionPreference = 'Stop'
$directory = Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString())
[IO.Directory]::CreateDirectory($directory) | Out-Null
$config = Join-Path $directory 'Engine.ini'
$common = "; settings`r`nRender.Particles.Enabled = false`r`nRender.DOF.Quality = 3`r`nUnknown.Key = keep`r`n"
$bistro = "Render.Grass.Enabled = false`r`nRender.Grass.Quality = 1`r`nRender.Wind.Enabled = false`r`nRender.Wind.Quality = 3`r`nRender.Fluid.Enabled = true`r`nRender.Fluid.Quality = 2`r`n"
try {
    [IO.File]::WriteAllText($config, $common + $bistro)
    & "$PSScriptRoot/../Remove-BistroRenderDefaults.ps1" -Path $config
    if ([IO.File]::ReadAllText($config) -cne $common) { throw 'Migration changed common settings or retained Bistro keys' }
    if ([IO.File]::ReadAllText("$config.bistro-backup") -cne $common + $bistro) { throw 'Original config was not backed up' }
    & "$PSScriptRoot/../Remove-BistroRenderDefaults.ps1" -Path $config
    if ([IO.File]::ReadAllText($config) -cne $common) { throw 'Second migration changed the result' }
    [IO.File]::WriteAllText($config, $common + $bistro)
    $rejected = $false
    try { & "$PSScriptRoot/../Remove-BistroRenderDefaults.ps1" -Path $config } catch { $rejected = $true }
    if (!$rejected -or [IO.File]::ReadAllText($config) -cne $common + $bistro) { throw 'Existing backup was overwritten' }
    'Bistro output config migration passed'
}
finally {
    [IO.File]::Delete($config)
    [IO.File]::Delete("$config.bistro-backup")
    [IO.Directory]::Delete($directory)
}
