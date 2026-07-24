[CmdletBinding()]
param(
    [string]$Installer = "",
    [string]$BuiltExecutable = "",
    [switch]$FullCycle
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$projectRoot = Split-Path -Parent $PSScriptRoot
$cmakeText = Get-Content `
    -LiteralPath (Join-Path $projectRoot "CMakeLists.txt") `
    -Raw
$versionMatch = [regex]::Match(
    $cmakeText,
    'project\s*\(\s*WindowsLaunchpad\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
if (-not $versionMatch.Success) {
    throw "Could not read the project version."
}
$version = $versionMatch.Groups[1].Value

if ([string]::IsNullOrWhiteSpace($Installer)) {
    $Installer = Join-Path $projectRoot (
        "dist\WindowsLaunchpad-{0}-Setup-x64.exe" -f $version)
}
if ([string]::IsNullOrWhiteSpace($BuiltExecutable)) {
    $BuiltExecutable =
        Join-Path $projectRoot `
            "out\build\launchpad-release\Launchpad.exe"
}

$Installer = [IO.Path]::GetFullPath($Installer)
$BuiltExecutable = [IO.Path]::GetFullPath($BuiltExecutable)
$installDirectory =
    Join-Path $env:LOCALAPPDATA "Programs\Windows Launchpad"
$installedExecutable =
    Join-Path $installDirectory "Launchpad.exe"
$dataDirectory =
    Join-Path $env:LOCALAPPDATA "WindowsLaunchpad"
$startMenuShortcut =
    Join-Path $env:APPDATA `
        "Microsoft\Windows\Start Menu\Programs\Launchpad.lnk"
$runKey =
    "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"

foreach ($requiredFile in @($Installer, $BuiltExecutable)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Required file was not found: $requiredFile"
    }
}
$createdTestData = $false
if (-not (Test-Path -LiteralPath $dataDirectory -PathType Container)) {
    $applicationsDirectory =
        Join-Path $dataDirectory "Applications"
    New-Item `
        -ItemType Directory `
        -Path $applicationsDirectory `
        -Force |
        Out-Null
    [IO.File]::WriteAllText(
        (Join-Path $applicationsDirectory "Installer QA.url"),
        "[InternetShortcut]`r`nURL=https://example.com/`r`n",
        [Text.UTF8Encoding]::new($false))
    $createdTestData = $true
}

function Get-TreeManifest([string]$Root) {
    $fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd("\")
    $prefix = $fullRoot + "\"
    return @(
        Get-ChildItem -LiteralPath $fullRoot -Force -Recurse |
            Sort-Object FullName |
            ForEach-Object {
                [pscustomobject][ordered]@{
                    Kind = if ($_.PSIsContainer) { "D" } else { "F" }
                    RelativePath =
                        $_.FullName.Substring($prefix.Length)
                    Length = if ($_.PSIsContainer) {
                        ""
                    } else {
                        [string]$_.Length
                    }
                    Sha256 = if ($_.PSIsContainer) {
                        ""
                    } else {
                        (Get-FileHash `
                            -LiteralPath $_.FullName `
                            -Algorithm SHA256).Hash
                    }
                }
            }
    )
}

function Assert-TreeUnchanged(
    [object[]]$Reference,
    [string]$Root,
    [string]$Stage) {
    $difference = @(
        Compare-Object `
            -ReferenceObject $Reference `
            -DifferenceObject (Get-TreeManifest $Root) `
            -Property Kind,RelativePath,Length,Sha256
    )
    if ($difference.Count -ne 0) {
        $difference | Format-Table -AutoSize
        throw "User data changed during: $Stage"
    }
}

function Get-InstalledLaunchpadProcesses {
    return @(
        Get-Process -Name Launchpad -ErrorAction SilentlyContinue |
            Where-Object {
                try {
                    $_.Path -and $_.Path.Equals(
                        $installedExecutable,
                        [StringComparison]::OrdinalIgnoreCase)
                } catch {
                    $false
                }
            }
    )
}

function Stop-InstalledLaunchpad([bool]$AllowForce) {
    $running = @(Get-InstalledLaunchpadProcesses)
    if ($running.Count -eq 0) {
        return
    }
    if (Test-Path `
            -LiteralPath $installedExecutable `
            -PathType Leaf) {
        try {
            Start-Process `
                -FilePath $installedExecutable `
                -ArgumentList "--shutdown" `
                -Wait
        } catch {
            if (-not $AllowForce) {
                throw
            }
        }
    }
    Start-Sleep -Milliseconds 500
    $remaining = @(Get-InstalledLaunchpadProcesses)
    if ($remaining.Count -gt 0 -and $AllowForce) {
        $remaining |
            Stop-Process -Force
        Start-Sleep -Milliseconds 500
        $remaining = @(Get-InstalledLaunchpadProcesses)
    }
    if ($remaining.Count -gt 0) {
        throw "Launchpad could not be stopped."
    }
}

function Invoke-Installer {
    $logPath = Join-Path $env:TEMP (
        "WindowsLaunchpad-setup-{0}.log" -f
        [guid]::NewGuid().ToString("N"))
    $arguments = @(
        "/VERYSILENT",
        "/SUPPRESSMSGBOXES",
        "/NORESTART",
        "/SP-",
        "/CLOSEAPPLICATIONS",
        "/TASKS=autostart",
        "/LOG=$logPath"
    )
    $result = Start-Process `
        -FilePath $Installer `
        -ArgumentList $arguments `
        -Wait `
        -PassThru
    if ($result.ExitCode -ne 0) {
        if (Test-Path -LiteralPath $logPath -PathType Leaf) {
            Get-Content -LiteralPath $logPath -Tail 80 |
                Write-Host
        }
        throw "Installer failed with exit code $($result.ExitCode)."
    }
}

function Get-LaunchpadUninstallEntry {
    return @(
        Get-ChildItem `
            "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall" |
            ForEach-Object {
                Get-ItemProperty -LiteralPath $_.PSPath
            } |
            Where-Object {
                $_.DisplayName -like "Windows Launchpad*"
            }
    )
}

function Assert-Installed {
    if (-not (Test-Path `
            -LiteralPath $installedExecutable `
            -PathType Leaf)) {
        throw "Launchpad.exe was not installed."
    }
    if ((Get-FileHash -LiteralPath $installedExecutable).Hash -ne
        (Get-FileHash -LiteralPath $BuiltExecutable).Hash) {
        throw "Installed Launchpad.exe does not match the release build."
    }
    if (-not (Test-Path `
            -LiteralPath $startMenuShortcut `
            -PathType Leaf)) {
        throw "The Start menu shortcut was not created."
    }
    $runValue = Get-ItemPropertyValue `
        -Path $runKey `
        -Name "WindowsLaunchpad" `
        -ErrorAction SilentlyContinue
    $expectedRunValue =
        '"{0}" --background' -f $installedExecutable
    if ($runValue -ne $expectedRunValue) {
        throw "The autostart registry value is incorrect: $runValue"
    }
    $entries = @(Get-LaunchpadUninstallEntry)
    if ($entries.Count -ne 1) {
        throw (
            "Expected one Installed apps entry, found " +
            $entries.Count)
    }
}

Stop-InstalledLaunchpad $true

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$qaDirectory =
    Join-Path `
        (Join-Path $env:USERPROFILE "Documents\WindowsLaunchpadInstallerQA") `
        $stamp
New-Item -ItemType Directory -Path $qaDirectory -Force |
    Out-Null
Copy-Item `
    -LiteralPath $dataDirectory `
    -Destination $qaDirectory `
    -Recurse `
    -Force

$baseline = @(Get-TreeManifest $dataDirectory)
$backupDirectory = Join-Path $qaDirectory "WindowsLaunchpad"
Assert-TreeUnchanged $baseline $backupDirectory "backup"
$baseline |
    Export-Csv `
        -LiteralPath (Join-Path $qaDirectory "baseline.csv") `
        -NoTypeInformation `
        -Encoding UTF8

Invoke-Installer
Assert-Installed
Assert-TreeUnchanged $baseline $dataDirectory "initial upgrade"

Start-Process `
    -FilePath $installedExecutable `
    -ArgumentList "--background"
Start-Sleep -Seconds 2
if (@(Get-InstalledLaunchpadProcesses).Count -ne 1) {
    throw "Launchpad did not start in the background."
}
if ($createdTestData) {
    $baseline = @(Get-TreeManifest $dataDirectory)
    $baseline |
        Export-Csv `
            -LiteralPath (
                Join-Path $qaDirectory "post-first-launch.csv") `
            -NoTypeInformation `
            -Encoding UTF8
} else {
    Assert-TreeUnchanged `
        $baseline `
        $dataDirectory `
        "first launch"
}

# Reinstall while the executable is mapped to verify update handling.
Invoke-Installer
Assert-Installed
Assert-TreeUnchanged $baseline $dataDirectory "running update"

Start-Process `
    -FilePath $installedExecutable `
    -ArgumentList "--background"
Start-Sleep -Seconds 1
Stop-InstalledLaunchpad $false
Assert-TreeUnchanged $baseline $dataDirectory "clean shutdown"

if ($FullCycle) {
    $entries = @(Get-LaunchpadUninstallEntry)
    $uninstallString = [string]$entries[0].UninstallString
    $uninstaller = if ($uninstallString.StartsWith('"')) {
        $uninstallString.Split('"')[1]
    } else {
        $uninstallString.Split(" ")[0]
    }
    $uninstallResult = Start-Process `
        -FilePath $uninstaller `
        -ArgumentList (
            "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART") `
        -Wait `
        -PassThru
    if ($uninstallResult.ExitCode -ne 0) {
        throw (
            "Uninstaller failed with exit code " +
            $uninstallResult.ExitCode)
    }
    if (Test-Path -LiteralPath $installedExecutable) {
        throw "Launchpad.exe remained after uninstall."
    }
    if (Test-Path -LiteralPath $startMenuShortcut) {
        throw "The Start menu shortcut remained after uninstall."
    }
    $runProperties = Get-ItemProperty `
        -Path $runKey `
        -ErrorAction SilentlyContinue
    if ($runProperties -and
        $runProperties.PSObject.Properties["WindowsLaunchpad"]) {
        throw "The autostart value remained after uninstall."
    }
    Assert-TreeUnchanged $baseline $dataDirectory "uninstall"

    Invoke-Installer
    Assert-Installed
    Assert-TreeUnchanged $baseline $dataDirectory "reinstall"
}

Start-Process `
    -FilePath $installedExecutable `
    -ArgumentList "--background"

Write-Host ""
Write-Host "Installer verification passed."
Write-Host "User-data backup:"
Write-Host $backupDirectory
