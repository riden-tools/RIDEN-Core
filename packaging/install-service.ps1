param(
    [Parameter(Mandatory=$true)][string]$Device,
    [string]$Executable = (Join-Path $PSScriptRoot "riden-cored.exe")
)
$Resolved = (Resolve-Path $Executable).Path
$Command = '"' + $Resolved + '" --service --device "' + $Device + '"'
# Run explicitly in an elevated PowerShell. The service remains read-only.
New-Service -Name "RIDEN-Core" -DisplayName "RIDEN-Core device daemon" -BinaryPathName $Command -StartupType Manual
Write-Host "Installed. Start-Service RIDEN-Core to start; Stop-Service RIDEN-Core to stop."
