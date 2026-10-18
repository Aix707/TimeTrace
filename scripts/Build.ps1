param([switch]$Package)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskVersion=([regex]::Match([IO.File]::ReadAllText("$taskRoot\CMakeLists.txt"),'project\(TimeTrace VERSION ([0-9.]+)')).Groups[1].Value
$taskTag='v'+$taskVersion.Replace('.','')
New-Item -ItemType Directory -Force -Path "$taskRoot\dist", "$taskRoot\test-output" | Out-Null
$taskVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $taskVs) { throw 'Visual C++ Build Tools are required.' }
$taskCmake = Join-Path $taskVs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $taskCmake -S $taskRoot -B "$taskRoot\build" -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE) { throw 'CMake configure failed.' }
& $taskCmake --build "$taskRoot\build" --config Release --parallel
if ($LASTEXITCODE) { throw 'Build failed.' }
& "$taskRoot\build\Release\timetrace_tests.exe" | Tee-Object -FilePath "$taskRoot\test-output\$taskTag-core-tests.log"
if ($LASTEXITCODE) { throw 'Tests failed.' }
& "$taskRoot\build\Release\integration_tests.exe" | Tee-Object -FilePath "$taskRoot\test-output\$taskTag-integration-tests.log"
if ($LASTEXITCODE) { throw 'Integration tests failed.' }
& "$taskRoot\build\Release\service_tests.exe" | Tee-Object -FilePath "$taskRoot\test-output\$taskTag-service-tests.log"
if ($LASTEXITCODE) { throw 'Service queue tests failed.' }
& "$taskRoot\build\Release\reminder_actions_probe.exe" "$taskRoot\build\Release\TimeTrace.exe" "$taskRoot\test-output\$taskTag-actions.json"
if ($LASTEXITCODE) { throw 'Notification action tests failed.' }
$taskBuildHash = (Get-FileHash -LiteralPath "$taskRoot\build\Release\TimeTrace.exe" -Algorithm SHA256).Hash
$taskDistHash = if (Test-Path -LiteralPath "$taskRoot\dist\TimeTrace.exe") { (Get-FileHash -LiteralPath "$taskRoot\dist\TimeTrace.exe" -Algorithm SHA256).Hash } else { '' }
if ($taskBuildHash -ne $taskDistHash) {
    Copy-Item -LiteralPath "$taskRoot\build\Release\TimeTrace.exe" -Destination "$taskRoot\dist\TimeTrace.exe" -Force
}
if ($Package) {
    $taskNsis = Get-ChildItem -LiteralPath "$taskRoot\tools" -Filter makensis.exe -Recurse | Select-Object -First 1 -ExpandProperty FullName
    if (-not $taskNsis) { throw 'Place the portable NSIS 3.11 build tool under tools, then retry.' }
    & $taskNsis /V2 "/DAPP_VERSION=$taskVersion" "$taskRoot\scripts\installer.nsi"
    if ($LASTEXITCODE) { throw 'Packaging failed.' }
}
