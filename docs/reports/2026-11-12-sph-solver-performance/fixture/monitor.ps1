param([string]$OutputPath)
$ErrorActionPreference = 'Stop'
$writer = [IO.StreamWriter]::new($OutputPath, $false)
try {
    while ($true) {
        $rows = @(Get-Process | Where-Object { $_.ProcessName -match '^(cl|link|MSBuild|Bistro.*|App|.*Tests|Sph.*|ngfx.*|UnrealEditor.*|ShaderCompileWorker|UnrealBuildTool|devenv|Rider.*)$' } | ForEach-Object {
            [ordered]@{pid=$_.Id; name=$_.ProcessName; cpu_s=$_.CPU; path=$_.Path}
        })
        $writer.WriteLine((@{utc=[DateTime]::UtcNow.ToString('o'); processes=$rows} | ConvertTo-Json -Compress -Depth 4))
        $writer.Flush()
        Start-Sleep -Milliseconds 1000
    }
} finally { $writer.Dispose() }
