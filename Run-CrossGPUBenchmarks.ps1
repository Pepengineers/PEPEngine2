$apps = 'App.Benchmark.Buffers', 'App.Benchmark.RowMajorTextures'
$executables = @{}
foreach ($app in $apps) {
    $exe = Join-Path $PSScriptRoot "Build\bin\x64\Release\$app\$app.exe"
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
        Write-Error "Executable not found at '$exe'. Build PEPEngine2.sln in Release | x64 first."
        exit 1
    }
    $executables[$app] = $exe
}

$deviceList = [IO.Path]::GetTempFileName()
try {
    $probe = Start-Process -FilePath $executables[$apps[0]] -ArgumentList '--list-devices' `
        -WorkingDirectory $PSScriptRoot -RedirectStandardOutput $deviceList -WindowStyle Hidden `
        -Wait -PassThru -ErrorAction Stop
    if ($probe.ExitCode -ne 0) {
        throw "DXGI adapter enumeration failed with exit code $($probe.ExitCode)."
    }
    $inventory = Get-Content -LiteralPath $deviceList -Raw -Encoding UTF8 | ConvertFrom-Json -ErrorAction Stop
    $devices = @($inventory.adapters)
} catch {
    Write-Error $_
    exit 1
} finally {
    Remove-Item -LiteralPath $deviceList -ErrorAction SilentlyContinue
}

$nameCounts = @{}
foreach ($device in $devices) {
    $nameCounts[$device.name]++
}
for ($i = 0; $i -lt $devices.Count; $i++) {
    $device = $devices[$i]
    $label = $device.name
    if ($nameCounts[$device.name] -gt 1) {
        $label += " (LUID $($device.luid))"
    }
    Write-Host "$($i + 1) - $label"
}
if ($devices.Count -lt 2) {
    Write-Error 'Two hardware Direct3D 12 adapters are required.'
    exit 1
}

function Read-DeviceId {
    param([string]$Prompt, [int]$ExcludedId = 0)

    while ($true) {
        $choice = Read-Host -Prompt $Prompt
        $id = 0
        if (-not [int]::TryParse($choice, [ref]$id) -or $id -lt 1 -or $id -gt $devices.Count) {
            Write-Warning "Enter a device ID from 1 to $($devices.Count)."
        } elseif ($id -eq $ExcludedId) {
            Write-Warning 'Primary and secondary must be different devices.'
        } else {
            return $id
        }
    }
}

$primaryId = Read-DeviceId -Prompt 'Select Primary Device ID'
$secondaryId = Read-DeviceId -Prompt 'Select Secondary Device ID' -ExcludedId $primaryId
$primary = $devices[$primaryId - 1]
$secondary = $devices[$secondaryId - 1]
$benchmarkArgs = '--adapters', $primary.luid, $secondary.luid
Write-Host "Primary: $primaryId - $($primary.name)"
Write-Host "Secondary: $secondaryId - $($secondary.name)"

$exitCode = 0

foreach ($app in $apps) {
    try {
        Write-Host "Starting $app"
        $process = Start-Process -FilePath $executables[$app] -ArgumentList $benchmarkArgs `
            -WorkingDirectory $PSScriptRoot -Wait -PassThru -ErrorAction Stop
        Write-Host "$app finished with exit code $($process.ExitCode)"

        if ($process.ExitCode -ne 0) {
            $exitCode = 1
        }
    } catch {
        Write-Error -Message "$($app): $($_.Exception.Message)" -ErrorAction Continue
        $exitCode = 1
    }
}

exit $exitCode
