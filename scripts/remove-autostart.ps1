[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$valueName = "WindowsLaunchpad"
$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"

if (Get-ItemProperty `
        -Path $runKey `
        -Name $valueName `
        -ErrorAction SilentlyContinue) {
    Remove-ItemProperty -Path $runKey -Name $valueName
    Write-Host "Windows Launchpad autostart removed."
} else {
    Write-Host "Windows Launchpad autostart was not configured."
}
