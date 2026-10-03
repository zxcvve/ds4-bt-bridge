<#
.SYNOPSIS
    Checks the build prerequisites for ds4bt.sys, then builds and test-signs it.

.DESCRIPTION
    Works with either Visual Studio 2022 + WDK, or the EWDK (run from its LaunchBuildEnv.cmd prompt).
    Every prerequisite is checked first and all problems are reported together, before anything is built.

.EXAMPLE
    .\build.ps1
    .\build.ps1 -Configuration Debug
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

# --- OS -------------------------------------------------------------------------------------------
if ($env:OS -ne 'Windows_NT') {
    Write-Error "This script must run on Windows (kernel drivers can't be built from WSL/Linux)."
}
if (-not [Environment]::Is64BitOperatingSystem) {
    Bad "64-bit Windows is required (the driver targets x64)."
} else {
    Ok "64-bit Windows $([Environment]::OSVersion.Version)"
}

# --- MSBuild and the Visual Studio / EWDK tree ----------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = $null
$vsPath  = $env:VSINSTALLDIR       # set inside the EWDK build prompt and VS developer prompts

if (-not $vsPath -and (Test-Path $vswhere)) {
    $vsPath = & $vswhere -latest -products * -version '[17.0,)' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if ($vsPath) {
    $candidate = Get-ChildItem -Path (Join-Path $vsPath 'MSBuild') -Filter MSBuild.exe -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\Bin\\(amd64\\)?MSBuild\.exe$' } | Select-Object -First 1
    if ($candidate) { $msbuild = $candidate.FullName }
}
if (-not $msbuild) {
    $onPath = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($onPath) { $msbuild = $onPath.Source }
}

if (-not $vsPath) {
    Bad "Visual Studio 2022 with the 'Desktop development with C++' workload not found (or run this from the EWDK's LaunchBuildEnv.cmd)."
} else {
    Ok "Visual Studio / EWDK tree: $vsPath"
}
if (-not $msbuild) {
    Bad "MSBuild.exe not found."
} else {
    Ok "MSBuild: $msbuild"
}

if ($vsPath) {
    # The WDK's Visual Studio integration provides this platform toolset.
    $toolset = Get-ChildItem -Path (Join-Path $vsPath 'MSBuild\Microsoft\VC') -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName 'Platforms\x64\PlatformToolsets\WindowsKernelModeDriver10.0' } |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($toolset) {
        Ok "WDK Visual Studio integration (WindowsKernelModeDriver10.0 toolset)"
    } else {
        Bad "WDK Visual Studio extension missing: install the WDK (it adds the WindowsKernelModeDriver10.0 toolset)."
    }

    # Driver projects link Spectre-mitigated runtime libs by default.
    $spectre = Get-ChildItem -Path (Join-Path $vsPath 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'lib\x64\spectre') } | Select-Object -First 1
    if ($spectre) {
        Ok "MSVC Spectre-mitigated libs ($($spectre.Name))"
    } else {
        Bad "MSVC Spectre-mitigated libs missing: Visual Studio Installer > Individual components > 'MSVC v143 - VS 2022 C++ x64/x86 Spectre-mitigated libs (Latest)'."
    }
}

# --- Windows SDK + WDK (same version) -------------------------------------------------------------
$kitsRoot = $null
if ($env:WindowsSdkDir -and (Test-Path $env:WindowsSdkDir)) {
    $kitsRoot = $env:WindowsSdkDir
} else {
    foreach ($key in 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots',
                     'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots') {
        $value = Get-ItemProperty -Path $key -Name KitsRoot10 -ErrorAction SilentlyContinue
        if ($value) { $kitsRoot = $value.KitsRoot10; break }
    }
}

$kitVersion = $null
if (-not $kitsRoot -or -not (Test-Path $kitsRoot)) {
    Bad "Windows Kits 10 not found: install the Windows SDK and the matching WDK."
} else {
    Ok "Windows Kits root: $kitsRoot"
    # A usable version has both SDK (um\windows.h) and WDK (km\ntddk.h + km libs) content.
    $versions = Get-ChildItem -Path (Join-Path $kitsRoot 'Include') -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^10\.0\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending
    foreach ($v in $versions) {
        $hasSdk = Test-Path (Join-Path $v.FullName 'um\windows.h')
        $hasWdk = (Test-Path (Join-Path $v.FullName 'km\ntddk.h')) -and
                  (Test-Path (Join-Path $kitsRoot "Lib\$($v.Name)\km\x64\ntoskrnl.lib"))
        if ($hasSdk -and $hasWdk) { $kitVersion = $v.Name; break }
    }
    if (-not $kitVersion) {
        $found = ($versions | ForEach-Object Name) -join ', '
        Bad "No Windows Kits version has both the SDK and the WDK installed (found: $found). Install the WDK whose version matches your SDK."
    } else {
        Ok "SDK + WDK version $kitVersion"

        $hidport = Join-Path $kitsRoot "Include\$kitVersion\km\hidport.h"
        if (-not (Test-Path $hidport)) {
            Bad "hidport.h not found in WDK $kitVersion."
        } else {
            $headers = Get-ChildItem -Path (Join-Path $kitsRoot "Include\$kitVersion") -Include *.h -Recurse -ErrorAction SilentlyContinue |
                Where-Object { $_.Directory.Name -in 'km', 'shared' }
            if (Select-String -Path $headers.FullName -Pattern 'define\s+IOCTL_UMDF_HID_SET_OUTPUT_REPORT' -Quiet) {
                Ok "hidport.h with IOCTL_UMDF_HID_SET_OUTPUT_REPORT"
            } else {
                Bad "IOCTL_UMDF_HID_SET_OUTPUT_REPORT not defined in WDK $kitVersion headers: use a newer WDK."
            }
        }
    }

    $kmdf = Join-Path $kitsRoot 'Include\wdf\kmdf\1.15\wdf.h'
    if (Test-Path $kmdf) {
        Ok "KMDF 1.15 headers"
    } else {
        Bad "KMDF 1.15 headers missing ($kmdf): the WDK install looks incomplete."
    }
}

# --- Sources --------------------------------------------------------------------------------------
$project = Join-Path $PSScriptRoot 'ds4bt.vcxproj'
foreach ($f in $project, 'src\driver.c', 'src\ds4_translate.c', 'src\ds4_usb_descriptor.h', 'src\ds4bt.inf') {
    $p = if ([IO.Path]::IsPathRooted($f)) { $f } else { Join-Path $PSScriptRoot $f }
    if (-not (Test-Path $p)) { Bad "Missing source file: $p" }
}

if ($problems.Count -gt 0) {
    Write-Host ""
    Write-Host "$($problems.Count) prerequisite problem(s); not building." -ForegroundColor Red
    exit 1
}

# --- Build ----------------------------------------------------------------------------------------
Write-Host ""
Write-Host "Building ds4bt ($Configuration|x64, SDK/WDK $kitVersion)..."
& $msbuild $project /nologo /m /v:minimal /restore:false `
    "/p:Configuration=$Configuration" '/p:Platform=x64' "/p:WindowsTargetPlatformVersion=$kitVersion"
if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed (MSBuild exit code $LASTEXITCODE)." -ForegroundColor Red
    exit $LASTEXITCODE
}

$outDir = Join-Path $PSScriptRoot "x64\$Configuration"
$package = Get-ChildItem -Path $outDir -Recurse -Include ds4bt.sys, ds4bt.inf, ds4bt.cat -ErrorAction SilentlyContinue |
    Where-Object { $_.Directory.Name -eq 'ds4bt' }
Write-Host ""
Write-Host "Build succeeded. Driver package:" -ForegroundColor Green
if ($package) {
    $package | ForEach-Object { Write-Host "  $($_.FullName)" }
    $pkgDir = $package[0].DirectoryName
} else {
    Write-Host "  (package folder not found under $outDir; look for ds4bt.sys there)"
    $pkgDir = $outDir
}
Write-Host ""
Write-Host "Install on a test machine (elevated prompt):"
Write-Host "  bcdedit /set testsigning on      # once, then reboot"
Write-Host "  pnputil /add-driver `"$pkgDir\ds4bt.inf`" /install"
