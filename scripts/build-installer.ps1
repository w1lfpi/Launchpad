[CmdletBinding()]
param(
    [string]$Version = "",
    [string]$BuildDirectory = "",
    [string]$OutputDirectory = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$projectRoot = Split-Path -Parent $PSScriptRoot
$cmakeLists = Join-Path $projectRoot "CMakeLists.txt"

if ([string]::IsNullOrWhiteSpace($Version)) {
    $projectText = Get-Content -LiteralPath $cmakeLists -Raw
    $versionMatch = [regex]::Match(
        $projectText,
        'project\s*\(\s*WindowsLaunchpad\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
    if (-not $versionMatch.Success) {
        throw "Could not read the project version from CMakeLists.txt."
    }
    $Version = $versionMatch.Groups[1].Value
}

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory =
        Join-Path $projectRoot "out\build\launchpad-release"
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $projectRoot "dist"
}

$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$executable = Join-Path $BuildDirectory "Launchpad.exe"
$installerScript =
    Join-Path $projectRoot "installer\Launchpad.iss"

function Find-FirstExistingFile([string[]]$Candidates) {
    foreach ($candidate in $Candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and
            (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return $null
}

function Import-VisualStudioEnvironment([string]$BatchFile) {
    $environmentLines = & $env:ComSpec /d /s /c (
        'call "{0}" >nul && set' -f $BatchFile)
    if ($LASTEXITCODE -ne 0) {
        throw "Could not initialize the Visual Studio build environment."
    }
    foreach ($line in $environmentLines) {
        $separator = $line.IndexOf("=")
        if ($separator -le 0) {
            continue
        }
        $name = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        Set-Item -LiteralPath "Env:$name" -Value $value
    }
}

$vswhere = Find-FirstExistingFile @(
    (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"),
    (Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe")
)
if (-not $vswhere) {
    throw "Visual Studio Installer (vswhere.exe) was not found."
}

$visualStudio = (
    & $vswhere -latest -products "*" `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
).Trim()
if ([string]::IsNullOrWhiteSpace($visualStudio)) {
    throw "Visual Studio with Desktop development with C++ was not found."
}

$vcvars = Join-Path $visualStudio "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path -LiteralPath $vcvars -PathType Leaf)) {
    throw "vcvars64.bat was not found: $vcvars"
}
Import-VisualStudioEnvironment $vcvars

$cmake = Find-FirstExistingFile @(
    (Join-Path $visualStudio "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"),
    (Get-Command cmake.exe -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty Source -First 1)
)
$ctest = Find-FirstExistingFile @(
    (Join-Path $visualStudio "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"),
    (Get-Command ctest.exe -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty Source -First 1)
)
$ninja = Find-FirstExistingFile @(
    (Join-Path $visualStudio "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"),
    (Get-Command ninja.exe -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty Source -First 1)
)
if (-not $cmake -or -not $ctest -or -not $ninja) {
    throw "CMake, CTest, or Ninja was not found."
}

if (-not $SkipBuild) {
    & $cmake `
        -S $projectRoot `
        -B $BuildDirectory `
        -G Ninja `
        "-DCMAKE_MAKE_PROGRAM=$ninja" `
        -DCMAKE_BUILD_TYPE=Release `
        -DBUILD_TESTING=ON
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed."
    }

    & $cmake --build $BuildDirectory --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "Release build failed."
    }

    & $ctest `
        --test-dir $BuildDirectory `
        --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        throw "Tests failed."
    }
}

if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Release executable was not found: $executable"
}

$executableVersion =
    (Get-Item -LiteralPath $executable).VersionInfo.ProductVersion
if ([string]::IsNullOrWhiteSpace($executableVersion) -or
    $executableVersion.Trim() -ne $Version) {
    throw (
        "Version mismatch: requested installer $Version, " +
        "but Launchpad.exe is $executableVersion.")
}

$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if ($dumpbin) {
    $dependencies = & $dumpbin.Source /dependents $executable
    $dynamicRuntime = $dependencies |
        Select-String -Pattern 'MSVCP\d+\.dll|VCRUNTIME\d+(?:_\d+)?\.dll'
    if ($dynamicRuntime) {
        throw (
            "Launchpad.exe still depends on the dynamic Visual C++ runtime: " +
            (($dynamicRuntime.Line.Trim() | Select-Object -Unique) -join ", "))
    }
}

$innoCompiler = Find-FirstExistingFile @(
    (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 7\ISCC.exe"),
    (Join-Path $env:ProgramFiles "Inno Setup 7\ISCC.exe"),
    (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 7\ISCC.exe")
)
if (-not $innoCompiler) {
    throw (
        "Inno Setup 7 was not found. Install it with: " +
        "winget install --id JRSoftware.InnoSetup.7 --exact --scope user")
}

New-Item -ItemType Directory -Path $OutputDirectory -Force |
    Out-Null

& $innoCompiler `
    "/DMyAppVersion=$Version" `
    "/DSourceExe=$executable" `
    "/DOutputDirectory=$OutputDirectory" `
    $installerScript
if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup compilation failed."
}

$installer = Join-Path $OutputDirectory (
    "WindowsLaunchpad-{0}-Setup-x64.exe" -f $Version)
if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) {
    throw "Installer was not produced: $installer"
}

$installerFile = Get-Item -LiteralPath $installer
$hash = Get-FileHash -LiteralPath $installer -Algorithm SHA256

Write-Host ""
Write-Host "Installer ready:"
Write-Host $installerFile.FullName
Write-Host ("Size: {0:N0} bytes" -f $installerFile.Length)
Write-Host "SHA256: $($hash.Hash)"
