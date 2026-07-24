[CmdletBinding()]
param(
    [string]$InstallDirectory = "",
    [switch]$RemoveUserData
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($InstallDirectory)) {
    $InstallDirectory = Join-Path `
        $env:LOCALAPPDATA `
        "Programs\Windows Launchpad"
}
$InstallDirectory = [IO.Path]::GetFullPath($InstallDirectory)
$installedExecutable = Join-Path $InstallDirectory "Launchpad.exe"
$dataDirectory = [IO.Path]::GetFullPath(
    (Join-Path $env:LOCALAPPDATA "WindowsLaunchpad"))
$applicationsDirectory = [IO.Path]::GetFullPath(
    (Join-Path $dataDirectory "Applications"))

if (Test-Path -LiteralPath $installedExecutable -PathType Leaf) {
    $shortcutProcess = Start-Process `
        -FilePath $installedExecutable `
        -ArgumentList "--remove-shortcuts" `
        -Wait `
        -PassThru
    if ($shortcutProcess.ExitCode -ne 0) {
        Write-Warning "Launchpad could not remove every shortcut."
    }
}

$shortcutPaths = @(
    (Join-Path `
        $env:APPDATA `
        "Microsoft\Windows\Start Menu\Programs\Launchpad.lnk"),
    (Join-Path `
        ([Environment]::GetFolderPath("Desktop")) `
        "Launchpad.lnk")
)
foreach ($shortcutPath in $shortcutPaths) {
    Remove-Item `
        -LiteralPath $shortcutPath `
        -Force `
        -ErrorAction SilentlyContinue
}

Get-Process -Name Launchpad -ErrorAction SilentlyContinue |
    ForEach-Object {
        try {
            if ($_.Path.Equals(
                    $installedExecutable,
                    [StringComparison]::OrdinalIgnoreCase)) {
                Stop-Process -Id $_.Id -Force
            }
        } catch {
            # A process may disappear between enumeration and inspection.
        }
    }
Start-Sleep -Milliseconds 250

$runKey =
    "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
$runValueName = "WindowsLaunchpad"
$runValue = Get-ItemPropertyValue `
    -Path $runKey `
    -Name $runValueName `
    -ErrorAction SilentlyContinue
if ($runValue -and
    $runValue.IndexOf(
        $installedExecutable,
        [StringComparison]::OrdinalIgnoreCase) -ge 0) {
    Remove-ItemProperty `
        -Path $runKey `
        -Name $runValueName
}

if ($RemoveUserData) {
    Remove-Item `
        -LiteralPath $InstallDirectory `
        -Recurse `
        -Force `
        -ErrorAction SilentlyContinue
    if (-not $dataDirectory.Equals(
            $InstallDirectory,
            [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item `
            -LiteralPath $dataDirectory `
            -Recurse `
            -Force `
            -ErrorAction SilentlyContinue
    }
    if (-not $applicationsDirectory.Equals(
            $InstallDirectory,
            [StringComparison]::OrdinalIgnoreCase) -and
        -not $applicationsDirectory.Equals(
            $dataDirectory,
            [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item `
            -LiteralPath $applicationsDirectory `
            -Recurse `
            -Force `
            -ErrorAction SilentlyContinue
    }
    Write-Host "Windows Launchpad and its user data were removed."
    return
}

foreach ($fileName in @(
        "Launchpad.exe",
        "uninstall-launchpad.ps1",
        "uninstall-launchpad.cmd")) {
    Remove-Item `
        -LiteralPath (Join-Path $InstallDirectory $fileName) `
        -Force `
        -ErrorAction SilentlyContinue
}
Write-Host "Windows Launchpad was removed."
Write-Host "Applications were preserved:"
Write-Host $applicationsDirectory
Write-Host "Layout and settings were preserved:"
Write-Host $dataDirectory
