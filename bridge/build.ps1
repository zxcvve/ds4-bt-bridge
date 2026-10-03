<#
.SYNOPSIS
    Checks the build prerequisites for ds4bridge.exe, then builds it. No WDK needed.

.DESCRIPTION
    Needs Visual Studio (2022 or newer) with "Desktop development with C++" (MSVC, Windows SDK, CMake tools)
    and Git (CMake uses it to fetch hidapi and ViGEmClient).
    Every prerequisite is checked first and all problems are reported together, before anything is built.

.EXAMPLE
    .\bridge\build.ps1
    .\bridge\build.ps1 -Configuration Debug
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$problems = New-Object System.Collections.Generic.List[string]
function Ok([string]$msg)   { Write-Host "  [ok]  $msg" -ForegroundColor Green }
function Bad([string]$msg)  { Write-Host "  [!!]  $msg" -ForegroundColor Red; $script:problems.Add($msg) }

Write-Host "Checking prerequisites..."

if ($env:OS -ne 'Windows_NT') {
    Write-Error "This script must run on Windows."
}

# --- Visual Studio with MSVC ----------------------------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath  = $env:VSINSTALLDIR       # set inside VS developer prompts
if (-not $vsPath -and (Test-Path $vswhere)) {
    $vsPath = & $vswhere -latest -products * -version '[17.0,)' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if ($vsPath) {
    Ok "Visual Studio with MSVC: $vsPath"
} else {
    Bad "Visual Studio 2022 or newer with the 'Desktop development with C++' workload not found."
}

# --- Windows SDK (user-mode headers only) ---------------------------------------------------------
$kitsRoot = $null
foreach ($key in 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots',
                 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots') {
    $value = Get-ItemProperty -Path $key -Name KitsRoot10 -ErrorAction SilentlyContinue
    if ($value) { $kitsRoot = $value.KitsRoot10; break }
}
$sdk = $null
if ($kitsRoot) {
    $sdk = Get-ChildItem -Path (Join-Path $kitsRoot 'Include') -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^10\.0\.\d+\.\d+$' -and (Test-Path (Join-Path $_.FullName 'um\cfgmgr32.h')) } |
        Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
}
if ($sdk) {
    Ok "Windows SDK $($sdk.Name)"
} else {
    Bad "Windows SDK not found: Visual Studio Installer > Individual components > 'Windows 11 SDK'."
}

# --- CMake: prefer the copy bundled with VS, it knows that VS version's generator -----------------
$cmake = $null
if ($vsPath) {
    $bundled = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path $bundled) { $cmake = $bundled }
}
if (-not $cmake) {
    $onPath = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($onPath) { $cmake = $onPath.Source }
}
if ($cmake) {
    Ok "CMake: $cmake"
} else {
    Bad "CMake not found: Visual Studio Installer > Individual components > 'C++ CMake tools for Windows'."
}

# --- Git (FetchContent clones hidapi and ViGEmClient) ---------------------------------------------
$git = $null
$onPath = Get-Command git.exe -ErrorAction SilentlyContinue
if ($onPath) {
    $git = $onPath.Source
} elseif ($vsPath) {
    $bundled = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer\Git\cmd\git.exe'
    if (Test-Path $bundled) { $git = $bundled }
}
if ($git) {
    Ok "Git: $git"
} else {
    Bad "Git not found: install Git for Windows (or VS Installer > Individual components > 'Git for Windows')."
}

if ($problems.Count -gt 0) {
    Write-Host ""
    Write-Host "$($problems.Count) prerequisite problem(s); not building." -ForegroundColor Red
    exit 1
}

# --- Build ----------------------------------------------------------------------------------------
$buildDir = Join-Path $PSScriptRoot 'build'
Write-Host ""
Write-Host "Configuring and building ds4bridge ($Configuration|x64)..."
& $cmake -S $PSScriptRoot -B $buildDir -A x64 "-DGIT_EXECUTABLE=$git"
if ($LASTEXITCODE -ne 0) {
    Write-Host "CMake configure failed (exit code $LASTEXITCODE)." -ForegroundColor Red
    exit $LASTEXITCODE
}
& $cmake --build $buildDir --config $Configuration
if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed (exit code $LASTEXITCODE)." -ForegroundColor Red
    exit $LASTEXITCODE
}

$exe = Join-Path $buildDir "$Configuration\ds4bridge.exe"
Write-Host ""
Write-Host "Build succeeded: $exe" -ForegroundColor Green
Write-Host "Needs ViGEmBus and HidHide installed; run it and follow the HidHide command it prints."
