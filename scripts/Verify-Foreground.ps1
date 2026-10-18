param([int]$Iterations=100,[int]$IntervalMs=600)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskDirectory = Join-Path $taskRoot ('test-output\foreground-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $taskDirectory | Out-Null
Copy-Item -LiteralPath "$taskRoot\build\Release\foreground_probe.exe" -Destination "$taskDirectory\ProbeA.exe"
Copy-Item -LiteralPath "$taskRoot\build\Release\foreground_probe.exe" -Destination "$taskDirectory\ProbeB.exe"
(Get-FileHash -LiteralPath "$taskRoot\dist\TimeTrace.exe" -Algorithm SHA256).Hash | Set-Content -LiteralPath "$taskDirectory\binary.sha256"
$taskApp = Start-Process -FilePath "$taskRoot\dist\TimeTrace.exe" -ArgumentList "--background --data-dir `"$taskDirectory\data`"" -WindowStyle Hidden -PassThru
try {
    Start-Sleep -Seconds 2
    $taskProbe = Start-Process -FilePath "$taskRoot\build\Release\foreground_probe.exe" -ArgumentList "--pair `"$taskDirectory\events.csv`" $Iterations $IntervalMs `"$taskDirectory\ProbeA.exe`" `"$taskDirectory\ProbeB.exe`"" -WindowStyle Hidden -PassThru -Wait
    if($taskProbe.ExitCode) { throw "Foreground probe failed: $($taskProbe.ExitCode)" }
} finally {
    Start-Process -FilePath "$taskRoot\dist\TimeTrace.exe" -ArgumentList "--stop --data-dir `"$taskDirectory\data`"" -WindowStyle Hidden -Wait
    $taskApp.WaitForExit(10000) | Out-Null
}
& python "$taskRoot\scripts\check_foreground.py" $taskDirectory
if($LASTEXITCODE) { throw 'Foreground attribution verification failed.' }
