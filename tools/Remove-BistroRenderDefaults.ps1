param([Parameter(Mandatory)][string]$Path)
$ErrorActionPreference = 'Stop'

$config = (Resolve-Path -LiteralPath $Path).ProviderPath
$text = [IO.File]::ReadAllText($config)
$updated = [regex]::Replace($text,
    '(?im)^[\t ]*Render\.(?:Grass|Wind|Fluid)\.(?:Enabled|Quality)[\t ]*=.*(?:\r?\n|$)', '')
if ($updated -ceq $text) { return }

# Preserve the original before changing a non-Bistro output configuration.
[IO.File]::Copy($config, "$config.bistro-backup", $false)
[IO.File]::WriteAllText($config, $updated, [Text.UTF8Encoding]::new($false))
