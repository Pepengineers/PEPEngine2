$exitCode = 0

foreach ($app in 'App.Benchmark.Buffers', 'App.Benchmark.RowMajorTextures') {
    $exe = Join-Path $PSScriptRoot "Build\bin\x64\Release\$app\$app.exe"

    try {
        if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
            throw "Executable not found at '$exe'. Build PEPEngine2.sln in Release | x64 first."
        }

        Write-Host "Starting $app"
        $process = Start-Process -FilePath $exe -WorkingDirectory $PSScriptRoot -Wait -PassThru -ErrorAction Stop
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
