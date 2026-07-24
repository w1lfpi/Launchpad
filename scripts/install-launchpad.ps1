[CmdletBinding()]
param(
    [string]$Executable = "",
    [string]$InstallDirectory = "",
    [switch]$DesktopShortcut,
    [switch]$NoAutostart,
    [switch]$DoNotStart
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($InstallDirectory)) {
    $InstallDirectory = Join-Path `
        $env:LOCALAPPDATA `
        "Programs\Windows Launchpad"
}

$projectRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Executable)) {
    $candidates = @(
        (Join-Path $PSScriptRoot "Launchpad.exe"),
        (Join-Path $projectRoot "Launchpad.exe"),
        (Join-Path $projectRoot "out\build\launchpad-release\Launchpad.exe"),
        (Join-Path $projectRoot "out\build\vs2022-x64\Release\Launchpad.exe"),
        (Join-Path $projectRoot "out\build\ninja-x64-release\Launchpad.exe")
    )
    $Executable = $candidates |
        Where-Object {
            Test-Path -LiteralPath $_ -PathType Leaf
        } |
        Select-Object -First 1
}

if ([string]::IsNullOrWhiteSpace($Executable) -or
    -not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "Launchpad.exe not found. Build Release or pass -Executable."
}

$sourceExecutable = (Resolve-Path -LiteralPath $Executable).Path
$InstallDirectory = [IO.Path]::GetFullPath($InstallDirectory)
$installedExecutable = Join-Path $InstallDirectory "Launchpad.exe"

$matchingPaths = @(
    $sourceExecutable,
    $installedExecutable
)
Get-Process -Name Launchpad -ErrorAction SilentlyContinue |
    ForEach-Object {
        try {
            $processPath = $_.Path
            if ($matchingPaths -contains $processPath) {
                Stop-Process -Id $_.Id -Force
            }
        } catch {
            # A process may disappear between enumeration and inspection.
        }
    }
Start-Sleep -Milliseconds 250

New-Item -ItemType Directory -Path $InstallDirectory -Force |
    Out-Null
if (-not $sourceExecutable.Equals(
        $installedExecutable,
        [StringComparison]::OrdinalIgnoreCase)) {
    Copy-Item `
        -LiteralPath $sourceExecutable `
        -Destination $installedExecutable `
        -Force
}

$sourceRoots = @(
    $PSScriptRoot,
    $projectRoot
)
$sourceRoot = $sourceRoots |
    Where-Object {
        Test-Path `
            -LiteralPath (Join-Path $_ "Applications") `
            -PathType Container
    } |
    Select-Object -First 1

$destinationApplications =
    Join-Path $InstallDirectory "Applications"
if (-not (Test-Path `
        -LiteralPath $destinationApplications `
        -PathType Container)) {
    if ($sourceRoot) {
        Copy-Item `
            -LiteralPath (Join-Path $sourceRoot "Applications") `
            -Destination $destinationApplications `
            -Recurse
    } else {
        New-Item `
            -ItemType Directory `
            -Path $destinationApplications |
            Out-Null
    }
}

$destinationLayout =
    Join-Path $InstallDirectory "LaunchpadLayout.store"
if ($sourceRoot -and
    -not (Test-Path -LiteralPath $destinationLayout)) {
    $sourceLayout = Join-Path $sourceRoot "LaunchpadLayout.store"
    if (Test-Path -LiteralPath $sourceLayout -PathType Leaf) {
        function ConvertTo-LaunchpadHex([string]$Value) {
            $builder = [Text.StringBuilder]::new(
                $Value.Length * 8)
            foreach ($character in $Value.ToCharArray()) {
                [void]$builder.AppendFormat(
                    "{0:X8}",
                    [int][char]$character)
            }
            return $builder.ToString()
        }

        $sourceApplications =
            Join-Path $sourceRoot "Applications"
        $sourcePrefix =
            ConvertTo-LaunchpadHex $sourceApplications
        $destinationPrefix =
            ConvertTo-LaunchpadHex $destinationApplications
        $layoutLines = [IO.File]::ReadAllLines(
            $sourceLayout,
            [Text.Encoding]::UTF8)

        for ($lineIndex = 0;
             $lineIndex -lt $layoutLines.Length;
             $lineIndex++) {
            $fields = $layoutLines[$lineIndex].Split("`t")
            if ($fields.Length -lt 2) {
                continue
            }
            $firstPathField = if ($fields[0] -eq "A") {
                1
            } elseif ($fields[0] -eq "F") {
                2
            } else {
                $fields.Length
            }
            for ($fieldIndex = $firstPathField;
                 $fieldIndex -lt $fields.Length;
                 $fieldIndex++) {
                if ($fields[$fieldIndex].StartsWith(
                        $sourcePrefix,
                        [StringComparison]::OrdinalIgnoreCase)) {
                    $fields[$fieldIndex] =
                        $destinationPrefix +
                        $fields[$fieldIndex].Substring(
                            $sourcePrefix.Length)
                }
            }
            $layoutLines[$lineIndex] = $fields -join "`t"
        }

        [IO.File]::WriteAllLines(
            $destinationLayout,
            $layoutLines,
            [Text.UTF8Encoding]::new($false))
    }
}

foreach ($scriptName in @(
        "uninstall-launchpad.ps1",
        "uninstall-launchpad.cmd")) {
    $scriptPath = Join-Path $PSScriptRoot $scriptName
    if (Test-Path -LiteralPath $scriptPath -PathType Leaf) {
        Copy-Item `
            -LiteralPath $scriptPath `
            -Destination (Join-Path $InstallDirectory $scriptName) `
            -Force
    }
}

$shortcutArguments = @("--create-shortcuts")
if ($DesktopShortcut) {
    $shortcutArguments += "--desktop-shortcut"
}
$shortcutProcess = Start-Process `
    -FilePath $installedExecutable `
    -ArgumentList $shortcutArguments `
    -Wait `
    -PassThru
if ($shortcutProcess.ExitCode -ne 0) {
    throw "Could not create Windows shortcuts."
}

$runKey =
    "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
$runValueName = "WindowsLaunchpad"
if ($NoAutostart) {
    Remove-ItemProperty `
        -Path $runKey `
        -Name $runValueName `
        -ErrorAction SilentlyContinue
} else {
    New-Item -Path $runKey -Force | Out-Null
    $autostartCommand =
        '"{0}" --background' -f $installedExecutable
    New-ItemProperty `
        -Path $runKey `
        -Name $runValueName `
        -Value $autostartCommand `
        -PropertyType String `
        -Force |
        Out-Null
}

if (-not $DoNotStart) {
    Start-Process `
        -FilePath $installedExecutable `
        -ArgumentList "--background"
}

Write-Host ""
Write-Host "Windows Launchpad installed:"
Write-Host $installedExecutable
Write-Host ""
Write-Host "Pin it: Start > All apps > Launchpad > Pin to taskbar."
Write-Host "No administrator rights were used."
