param([int]$DurationSeconds = 1800, [int]$WarmupSeconds = 120, [string]$Name = 'background')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskDirectory = Join-Path $taskRoot "test-output\$Name-data"
$taskPrefix = Join-Path $taskRoot "test-output\$Name"
New-Item -ItemType Directory -Force -Path $taskDirectory | Out-Null
$taskBinaryDirectory = Join-Path $taskRoot "test-output\$Name-bin"
New-Item -ItemType Directory -Force -Path $taskBinaryDirectory | Out-Null
Copy-Item -LiteralPath "$taskRoot\dist\TimeTrace.exe" -Destination "$taskBinaryDirectory\TimeTrace.exe" -Force
$taskApp = Start-Process -FilePath "$taskBinaryDirectory\TimeTrace.exe" -ArgumentList "--background --data-dir `"$taskDirectory`"" -WindowStyle Hidden -PassThru
[PSCustomObject]@{ Started=(Get-Date -Format o); ApplicationPID=$taskApp.Id; DurationSeconds=$DurationSeconds; WarmupSeconds=$WarmupSeconds; SHA256=(Get-FileHash -LiteralPath "$taskBinaryDirectory\TimeTrace.exe" -Algorithm SHA256).Hash } | ConvertTo-Json | Set-Content -LiteralPath "$taskPrefix-status.json"
try {
    & "$taskRoot\build\Release\resource_probe.exe" $taskApp.Id $DurationSeconds $WarmupSeconds $taskPrefix
    if ($LASTEXITCODE) { throw 'Resource measurement did not complete successfully.' }
} finally {
    Start-Process -FilePath "$taskBinaryDirectory\TimeTrace.exe" -ArgumentList "--stop --data-dir `"$taskDirectory`"" -WindowStyle Hidden -Wait
    & python "$taskRoot\scripts\build_report.py"
}
