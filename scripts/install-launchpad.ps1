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
$dataDirectory = [IO.Path]::GetFullPath(
    (Join-Path $env:LOCALAPPDATA "WindowsLaunchpad"))
$destinationApplications = [IO.Path]::GetFullPath(
    (Join-Path $dataDirectory "Applications"))
$desktopApplications = [IO.Path]::GetFullPath(
    (Join-Path `
        ([Environment]::GetFolderPath("Desktop")) `
        "Launchpad Applications"))
$destinationLayout =
    Join-Path $dataDirectory "LaunchpadLayout.store"
$catalogMarker =
    Join-Path $dataDirectory ".catalog-initialized"
$legacyApplications =
    Join-Path $InstallDirectory "Applications"
$legacyLayout =
    Join-Path $InstallDirectory "LaunchpadLayout.store"

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

$storedCatalogPath = $null
if (Test-Path -LiteralPath $catalogMarker -PathType Leaf) {
    try {
        $storedCatalogPath = [IO.Path]::GetFullPath(
            [IO.File]::ReadAllText($catalogMarker).Trim())
    } catch {
        $storedCatalogPath = $null
    }
}
$storedCatalogTrusted =
    -not [string]::IsNullOrWhiteSpace($storedCatalogPath) -and
    ($storedCatalogPath.Equals(
            $destinationApplications,
            [StringComparison]::OrdinalIgnoreCase) -or
     $storedCatalogPath.Equals(
            $desktopApplications,
            [StringComparison]::OrdinalIgnoreCase))
$catalogInitialized =
    $storedCatalogTrusted -and
    $storedCatalogPath.Equals(
        $destinationApplications,
        [StringComparison]::OrdinalIgnoreCase)

if (-not $catalogInitialized) {
    $destinationHadContent =
        (Test-Path `
            -LiteralPath $destinationApplications `
            -PathType Container) -and
        (@(Get-ChildItem `
            -LiteralPath $destinationApplications `
            -Force `
            -ErrorAction SilentlyContinue).Count -gt 0)

    New-Item -ItemType Directory -Path $dataDirectory -Force |
        Out-Null
    New-Item -ItemType Directory -Path $destinationApplications -Force |
        Out-Null

    function Copy-MissingLaunchpadItems(
        [string]$Source,
        [string]$Destination,
        [bool]$OverwriteExisting = $false) {
        if (-not (Test-Path -LiteralPath $Source -PathType Container) -or
            $Source.Equals(
                $Destination,
                [StringComparison]::OrdinalIgnoreCase)) {
            return
        }

        foreach ($item in Get-ChildItem -LiteralPath $Source -Force) {
            $target = Join-Path $Destination $item.Name
            if ($item.PSIsContainer) {
                if (($item.Attributes -band
                        [IO.FileAttributes]::ReparsePoint) -ne 0) {
                    continue
                }
                New-Item -ItemType Directory -Path $target -Force |
                    Out-Null
                Copy-MissingLaunchpadItems `
                    -Source $item.FullName `
                    -Destination $target `
                    -OverwriteExisting $OverwriteExisting
            } elseif ($OverwriteExisting -or
                -not (Test-Path -LiteralPath $target)) {
                Copy-Item `
                    -LiteralPath $item.FullName `
                    -Destination $target `
                    -Force
            }
        }
    }

    function Test-LaunchpadCatalogCopy(
        [string]$Source,
        [string]$Destination) {
        foreach ($sourceFile in Get-ChildItem `
                -LiteralPath $Source `
                -Recurse `
                -Force `
                -File) {
            $relativePath = $sourceFile.FullName.Substring(
                $Source.TrimEnd("\").Length).TrimStart("\")
            $destinationFile =
                Join-Path $Destination $relativePath
            if (-not (Test-Path `
                    -LiteralPath $destinationFile `
                    -PathType Leaf)) {
                return $false
            }
            if ((Get-FileHash `
                    -LiteralPath $sourceFile.FullName `
                    -Algorithm SHA256).Hash -ne
                (Get-FileHash `
                    -LiteralPath $destinationFile `
                    -Algorithm SHA256).Hash) {
                return $false
            }
        }
        return $true
    }

    $authoritativeCatalog = $null
    if ($storedCatalogTrusted -and
        -not $storedCatalogPath.Equals(
            $destinationApplications,
            [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path `
            -LiteralPath $storedCatalogPath `
            -PathType Container)) {
        $authoritativeCatalog = $storedCatalogPath
    } elseif (Test-Path `
            -LiteralPath $desktopApplications `
            -PathType Container) {
        $authoritativeCatalog = $desktopApplications
    }

    if ($authoritativeCatalog) {
        $applicationSources = @($authoritativeCatalog)
    } elseif ($destinationHadContent) {
        $applicationSources = @()
    } else {
        $applicationSources = @($legacyApplications)
        if ($sourceRoot) {
            $applicationSources +=
                Join-Path $sourceRoot "Applications"
        }
    }

    foreach ($applicationSource in $applicationSources) {
        Copy-MissingLaunchpadItems `
            -Source ([IO.Path]::GetFullPath($applicationSource)) `
            -Destination $destinationApplications `
            -OverwriteExisting (
                $authoritativeCatalog -and
                $applicationSource.Equals(
                    $authoritativeCatalog,
                    [StringComparison]::OrdinalIgnoreCase))
    }
    if ($authoritativeCatalog -and
        -not (Test-LaunchpadCatalogCopy `
            -Source $authoritativeCatalog `
            -Destination $destinationApplications)) {
        throw "Launchpad catalog migration could not be verified."
    }

    $layoutCandidates = @(
        $destinationLayout,
        $legacyLayout
    )
    if ($sourceRoot) {
        $layoutCandidates +=
            Join-Path $sourceRoot "LaunchpadLayout.store"
    }
    $layoutSource = $layoutCandidates |
        Select-Object -Unique |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1

    if ($layoutSource) {
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

        $sourcePrefixes = $applicationSources |
            ForEach-Object {
                ConvertTo-LaunchpadHex (
                    [IO.Path]::GetFullPath($_).TrimEnd("\") + "\")
            } |
            Select-Object -Unique
        $destinationPrefix =
            ConvertTo-LaunchpadHex (
                $destinationApplications.TrimEnd("\") + "\")
        $layoutLines = [IO.File]::ReadAllLines(
            $layoutSource,
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
                foreach ($sourcePrefix in $sourcePrefixes) {
                    if ($fields[$fieldIndex].StartsWith(
                            $sourcePrefix,
                            [StringComparison]::OrdinalIgnoreCase)) {
                        $fields[$fieldIndex] =
                            $destinationPrefix +
                            $fields[$fieldIndex].Substring(
                                $sourcePrefix.Length)
                        break
                    }
                }
            }
            $layoutLines[$lineIndex] = $fields -join "`t"
        }

        $temporaryLayout =
            $destinationLayout + ".migration-" + $PID + ".tmp"
        try {
            [IO.File]::WriteAllLines(
                $temporaryLayout,
                $layoutLines,
                [Text.UTF8Encoding]::new($false))
            Move-Item `
                -LiteralPath $temporaryLayout `
                -Destination $destinationLayout `
                -Force
        } finally {
            Remove-Item `
                -LiteralPath $temporaryLayout `
                -Force `
                -ErrorAction SilentlyContinue
        }
    }

    $temporaryMarker =
        $catalogMarker + ".migration-" + $PID + ".tmp"
    [IO.File]::WriteAllText(
        $temporaryMarker,
        $destinationApplications,
        [Text.UTF8Encoding]::new($false))
    Move-Item `
        -LiteralPath $temporaryMarker `
        -Destination $catalogMarker `
        -Force
    (Get-Item -LiteralPath $catalogMarker).Attributes =
        [IO.FileAttributes]::Hidden

    if ($authoritativeCatalog -and
        $authoritativeCatalog.Equals(
            $desktopApplications,
            [StringComparison]::OrdinalIgnoreCase)) {
        try {
            Remove-Item `
                -LiteralPath $desktopApplications `
                -Recurse `
                -Force
        } catch {
            Write-Warning (
                "The migrated Desktop catalog could not be removed: " +
                $desktopApplications)
        }
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
Write-Host "Applications:"
Write-Host $destinationApplications
Write-Host "Layout and settings:"
Write-Host $dataDirectory
Write-Host ""
Write-Host "Pin it: Start > All apps > Launchpad > Pin to taskbar."
Write-Host "No administrator rights were used."
