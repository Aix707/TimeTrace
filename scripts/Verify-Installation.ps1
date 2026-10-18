$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskVersion=([regex]::Match([IO.File]::ReadAllText("$taskRoot\CMakeLists.txt"),'project\(TimeTrace VERSION ([0-9.]+)')).Groups[1].Value
$taskRegistry = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\TimeTrace'
if(Test-Path -LiteralPath $taskRegistry) { throw 'An existing TimeTrace installation is registered. Use a test machine or uninstall it first.' }
if((Get-Process TimeTrace -ErrorAction SilentlyContinue) -or (Test-Path -LiteralPath "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\TimeTrace.lnk")) { throw 'A real TimeTrace process or shortcut exists. Run this installer test in an isolated Windows account; it must not alter the user installation.' }
$taskTarget = Join-Path $taskRoot 'test-output\installed'
$taskInstall = Start-Process -FilePath "$taskRoot\dist\TimeTrace-$taskVersion-Setup.exe" -ArgumentList "/S /D=$taskTarget" -WindowStyle Hidden -Wait -PassThru
if($taskInstall.ExitCode) { throw 'Silent install failed.' }
$taskFiles = @(Get-ChildItem -LiteralPath $taskTarget -File)
$taskReport = [PSCustomObject]@{ InstallerExit=$taskInstall.ExitCode; InstalledBytes=($taskFiles | Measure-Object Length -Sum).Sum; Files=@($taskFiles | Select-Object Name,Length); Shortcut=(Test-Path -LiteralPath "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\TimeTrace.lnk"); ProtocolCommand=(Get-ItemProperty -LiteralPath 'HKCU:\Software\Classes\timetrace\shell\open\command').'(default)' }
$taskReport | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$taskRoot\test-output\install.json"
$taskUninstall = Start-Process -FilePath "$taskTarget\Uninstall.exe" -ArgumentList '/S' -WindowStyle Hidden -Wait -PassThru
for($taskAttempt=0; $taskAttempt -lt 50 -and (Test-Path -LiteralPath "$taskTarget\TimeTrace.exe"); $taskAttempt++) { Start-Sleep -Milliseconds 100 }
[PSCustomObject]@{ UninstallerExit=$taskUninstall.ExitCode; ApplicationRemoved= -not (Test-Path -LiteralPath "$taskTarget\TimeTrace.exe"); ShortcutRemoved= -not (Test-Path -LiteralPath "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\TimeTrace.lnk"); RegistrationRemoved= -not (Test-Path -LiteralPath $taskRegistry); UserDataPreserved=(Test-Path -LiteralPath "$env:LOCALAPPDATA\TimeTrace") } | ConvertTo-Json | Set-Content -LiteralPath "$taskRoot\test-output\uninstall.json"
Get-Content -LiteralPath "$taskRoot\test-output\install.json","$taskRoot\test-output\uninstall.json"
