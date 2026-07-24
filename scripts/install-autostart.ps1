[CmdletBinding()]
param(
    [string]$Executable = ""
)

$ErrorActionPreference = "Stop"
$valueName = "WindowsLaunchpad"
$runKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"

if ([string]::IsNullOrWhiteSpace($Executable)) {
    $projectRoot = Split-Path -Parent $PSScriptRoot
    $candidates = @(
        (Join-Path `
            $env:LOCALAPPDATA `
            "Programs\Windows Launchpad\Launchpad.exe"),
        (Join-Path $projectRoot "out\build\launchpad-release\Launchpad.exe"),
        (Join-Path $projectRoot "out\build\vs2022-x64\Release\Launchpad.exe"),
        (Join-Path $projectRoot "out\build\ninja-x64-release\Launchpad.exe")
    )
    $Executable = $candidates |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
}

if ([string]::IsNullOrWhiteSpace($Executable) -or
    -not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "Launchpad.exe not found. Pass -Executable with the full path."
}

$resolved = (Resolve-Path -LiteralPath $Executable).Path
$command = '"{0}" --background' -f $resolved

New-Item -Path $runKey -Force | Out-Null
New-ItemProperty `
    -Path $runKey `
    -Name $valueName `
    -Value $command `
    -PropertyType String `
    -Force | Out-Null

Write-Host "Autostart enabled for the current user:"
Write-Host $command
Write-Host "After sign-in press Win+Alt+Space to show Launchpad."
