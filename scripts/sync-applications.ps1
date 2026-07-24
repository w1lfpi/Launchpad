[CmdletBinding()]
param(
    [string]$Destination = "",
    [switch]$CleanPreviousStartMenuImports
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = Join-Path (Split-Path -Parent $PSScriptRoot) "Applications"
}

New-Item -ItemType Directory -Path $Destination -Force | Out-Null

$roots = @(
    [Environment]::GetFolderPath("Programs"),
    [Environment]::GetFolderPath("CommonPrograms")
) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }

$supported = @(".lnk", ".url", ".appref-ms", ".exe")
$manifestPath = Join-Path $Destination ".launchpad-imports.json"
$imported = 0
$skipped = 0
$filtered = 0
$removed = 0

$excludedNamePattern = @(
    "administrative tools",
    "application verifier",
    "command prompt",
    "component services",
    "computer management",
    "control panel",
    "debuggable package manager",
    "developer powershell",
    "dfrgui",
    "disk cleanup",
    "documentation",
    "event viewer",
    "faq",
    "gpuview",
    "installer",
    "magnify",
    "narrator",
    "native tools",
    "odbc data sources",
    "on-screen keyboard",
    "performance analyzer",
    "performance monitor",
    "performance recorder",
    "print management",
    "recoverydrive",
    "registry editor",
    "release notes",
    "resource monitor",
    "sample desktop apps",
    "sample uwp apps",
    "security configuration management",
    "services",
    "software development kit",
    "steps recorder",
    "system configuration",
    "system information",
    "task scheduler",
    "tools for desktop apps",
    "tools for uwp apps",
    "windows defender firewall",
    "windows powershell"
) -join "|"

$excludedExactNames = @(
    "character map",
    "firefox private browsing",
    "live captions",
    "livecaptions",
    "run",
    "task manager",
    "voice access",
    "voiceaccess"
)

function Test-IsLaunchpadApplication {
    param([System.IO.FileInfo]$Item)

    if ($Item.FullName -like "*\Parallels Shared Applications\*") {
        return $false
    }

    $normalizedName = $Item.BaseName.ToLowerInvariant()
    if ($normalizedName -match $excludedNamePattern) {
        return $false
    }

    if ($excludedExactNames -contains $normalizedName) {
        return $false
    }

    if ($normalizedName -match "(^|[_ ])(arm64|x86|x64)([_ ]|$)" -and
        $normalizedName -match "(tools|powershell)") {
        return $false
    }

    return $true
}

$allStartMenuItems = Get-ChildItem `
    -LiteralPath $roots `
    -Recurse `
    -File `
    -ErrorAction SilentlyContinue |
    Where-Object {
        $supported -contains $_.Extension.ToLowerInvariant()
    }

if ($CleanPreviousStartMenuImports) {
    foreach ($source in $allStartMenuItems) {
        $existing = Join-Path $Destination $source.Name
        if (-not (Test-Path -LiteralPath $existing -PathType Leaf)) {
            continue
        }
        $sourceHash = (Get-FileHash -LiteralPath $source.FullName -Algorithm SHA256).Hash
        $existingHash = (Get-FileHash -LiteralPath $existing -Algorithm SHA256).Hash
        if ($sourceHash -eq $existingHash) {
            Remove-Item -LiteralPath $existing -Force
            $removed++
        }
    }
}

if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
    $previousImports = Get-Content -LiteralPath $manifestPath -Raw |
        ConvertFrom-Json
    foreach ($entry in $previousImports) {
        $name = if ($entry -is [string]) { $entry } else { $entry.Name }
        $existing = Join-Path $Destination $name
        if (Test-Path -LiteralPath $existing -PathType Leaf) {
            $canRemove = $entry -is [string]
            if (-not $canRemove) {
                $existingHash = (
                    Get-FileHash -LiteralPath $existing -Algorithm SHA256
                ).Hash
                $canRemove = $existingHash -eq $entry.Hash
            }
            if ($canRemove) {
                Remove-Item -LiteralPath $existing -Force
                $removed++
            }
        }
    }
}

$items = $allStartMenuItems |
    Where-Object {
        if (Test-IsLaunchpadApplication $_) {
            return $true
        }
        $script:filtered++
        return $false
    } |
    Sort-Object BaseName, FullName

$managedItems = [System.Collections.Generic.List[object]]::new()

foreach ($item in $items) {
    $targetName = $item.Name
    $target = Join-Path $Destination $targetName

    if (Test-Path -LiteralPath $target) {
        $sameSource = (Get-Item -LiteralPath $target).Length -eq $item.Length
        if (-not $sameSource) {
            $targetName = "{0} - {1}{2}" -f `
                $item.BaseName, `
                $item.Directory.Name, `
                $item.Extension
            $target = Join-Path $Destination $targetName
        }
    }

    if (Test-Path -LiteralPath $target) {
        $skipped++
        continue
    }

    Copy-Item -LiteralPath $item.FullName -Destination $target
    $managedItems.Add([PSCustomObject]@{
        Name = $targetName
        Hash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    })
    $imported++
}

$managedItems |
    ConvertTo-Json |
    Set-Content -LiteralPath $manifestPath -Encoding UTF8

Write-Host ("Applications folder: " + (Resolve-Path $Destination).Path)
Write-Host ("Imported: " + $imported)
Write-Host ("Already present: " + $skipped)
Write-Host ("Filtered non-app entries: " + $filtered)
Write-Host ("Removed old managed imports: " + $removed)
