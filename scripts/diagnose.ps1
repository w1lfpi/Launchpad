[CmdletBinding()]
param(
    [string]$Executable = ""
)

$ErrorActionPreference = "Continue"
Add-Type -AssemblyName System.Windows.Forms
Write-Host "Windows Launchpad diagnostics"
Write-Host "-----------------------------"
Write-Host ("OS: " + [Environment]::OSVersion.VersionString)
Write-Host ("PowerShell: " + $PSVersionTable.PSVersion)
Write-Host ("DPI (primary monitor): " +
    [System.Windows.Forms.SystemInformation]::PrimaryMonitorSize)

$userPrograms = [Environment]::GetFolderPath("Programs")
$commonPrograms = [Environment]::GetFolderPath("CommonPrograms")
Write-Host ("User Start Menu: " + $userPrograms)
Write-Host ("Common Start Menu: " + $commonPrograms)

$extensions = @(".lnk", ".url", ".appref-ms", ".exe")
$items = Get-ChildItem `
    -LiteralPath @($userPrograms, $commonPrograms) `
    -Recurse `
    -File `
    -ErrorAction SilentlyContinue |
    Where-Object {
        $extensions -contains $_.Extension.ToLowerInvariant() -and
        $_.FullName -notlike "*\Parallels Shared Applications\*"
    }
Write-Host ("Launchable Start Menu items: " + @($items).Count)

$applicationsDirectory = Join-Path `
    (Join-Path $env:LOCALAPPDATA "WindowsLaunchpad") `
    "Applications"
$catalogItems = Get-ChildItem `
    -LiteralPath $applicationsDirectory `
    -Recurse `
    -File `
    -ErrorAction SilentlyContinue |
    Where-Object { $extensions -contains $_.Extension.ToLowerInvariant() }
Write-Host ("Applications folder: " + $applicationsDirectory)
Write-Host ("Launchpad catalog items: " + @($catalogItems).Count)

if (-not [string]::IsNullOrWhiteSpace($Executable)) {
    if (Test-Path -LiteralPath $Executable -PathType Leaf) {
        $file = Get-Item -LiteralPath $Executable
        Write-Host ("Executable: " + $file.FullName)
        Write-Host ("Executable size: " + $file.Length + " bytes")
        Write-Host ("Executable timestamp: " + $file.LastWriteTime)
    } else {
        Write-Warning "Executable was not found: $Executable"
    }
}

$runValue = $null
try {
    $runValue = Get-ItemPropertyValue `
        -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run" `
        -Name "WindowsLaunchpad" `
        -ErrorAction Stop
} catch {
    $runValue = $null
}
Write-Host ("Autostart: " +
    $(if ($runValue) { $runValue } else { "not configured" }))
