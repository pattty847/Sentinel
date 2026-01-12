<# 
    Sentinel Windows Setup Script
    -----------------------------
    One-shot helper for configuring a fresh Windows dev environment.

    Responsibilities:
      - Verify MSVC Build Tools + Windows 10 SDK
      - Verify VCPKG_ROOT and QT_MSVC
      - Run vcpkg install (manifest-based) for all dependencies
      - Generate CMakeUserPresets.json if missing
      - Configure CMake using the "windows-msvc-vs" preset
#>

param(
    [switch]$SkipVcpkgInstall
)

Write-Host "=== Sentinel Windows Setup ===`n"

$ErrorActionPreference = "Stop"

function Fail($message) {
    Write-Error $message
    exit 1
}

# ---------------------------------------------------------------------------
# Locate repo root (script lives in scripts/)
# ---------------------------------------------------------------------------
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Resolve-Path (Join-Path $scriptDir "..")
Set-Location $repoRoot

Write-Host "Repo root: $repoRoot"

# ---------------------------------------------------------------------------
# Validate toolchain: MSVC Build Tools (SDK is implied but not hard-required)
# ---------------------------------------------------------------------------
Write-Host "`nChecking for MSVC Build Tools..."

$vswhere = Join-Path "${env:ProgramFiles(x86)}" "Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path $vswhere)) {
    Fail "vswhere.exe not found. Install Visual Studio 2022 Build Tools or full VS 2022 (including C++ workload)."
}

$vsInstallPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath 2>$null

if (-not $vsInstallPath) {
    Fail "No suitable Visual Studio 2022 installation found with C++ tools. Install 'Desktop development with C++' or the Build Tools workload."
}

Write-Host "Using MSVC from: $vsInstallPath"

# ---------------------------------------------------------------------------
# Validate VCPKG_ROOT and QT_MSVC
# ---------------------------------------------------------------------------
Write-Host "`nChecking environment variables..."

if (-not $env:VCPKG_ROOT) {
    Write-Warning "VCPKG_ROOT is not set."
    Write-Host "Set it using (example):"
    Write-Host '  setx VCPKG_ROOT "C:\dev\vcpkg"'
    Fail "VCPKG_ROOT must be set before running this script."
}

if (-not (Test-Path $env:VCPKG_ROOT)) {
    Fail "VCPKG_ROOT points to a non-existent path: $env:VCPKG_ROOT"
}

$vcpkgExe = Join-Path $env:VCPKG_ROOT "vcpkg.exe"
if (-not (Test-Path $vcpkgExe)) {
    Fail "vcpkg.exe not found under VCPKG_ROOT ($env:VCPKG_ROOT). Make sure vcpkg is installed correctly."
}

if (-not $env:QT_MSVC) {
    Write-Warning "QT_MSVC is not set."
    Write-Host "Set it to your Qt installation path, e.g.:"
    Write-Host '  setx QT_MSVC "C:\Qt\6.9.3\msvc2022_64"'
    Fail "QT_MSVC must be set before running this script."
}

if (-not (Test-Path $env:QT_MSVC)) {
    Fail "QT_MSVC points to a non-existent path: $env:QT_MSVC"
}

$windeployqt = Join-Path $env:QT_MSVC "bin\windeployqt.exe"
if (-not (Test-Path $windeployqt)) {
    Fail "windeployqt.exe not found at: $windeployqt"
}

Write-Host "Environment looks good."
Write-Host "  VCPKG_ROOT = $($env:VCPKG_ROOT)"
Write-Host "  QT_MSVC    = $($env:QT_MSVC)"

# ---------------------------------------------------------------------------
# vcpkg manifest install
# ---------------------------------------------------------------------------
if (-not $SkipVcpkgInstall) {
    Write-Host "`nRunning vcpkg manifest install (triplet: x64-windows)..."
    & $vcpkgExe install --triplet x64-windows
    if ($LASTEXITCODE -ne 0) {
        Fail "vcpkg install failed. See output above."
    }
} else {
    Write-Host "`nSkipping vcpkg install (--SkipVcpkgInstall specified)."
}

# ---------------------------------------------------------------------------
# Generate CMakeUserPresets.json if missing
# ---------------------------------------------------------------------------
$userPresetsPath = Join-Path $repoRoot "CMakeUserPresets.json"

if (-not (Test-Path $userPresetsPath)) {
    Write-Host "`nCreating minimal CMakeUserPresets.json..."
    $userPresets = @{
        version = 3
        configurePresets = @()
        buildPresets     = @()
        testPresets      = @()
    } | ConvertTo-Json -Depth 5

    $userPresets | Out-File -FilePath $userPresetsPath -Encoding UTF8 -NoNewline
} else {
    Write-Host "`nCMakeUserPresets.json already exists; leaving it untouched."
}

# ---------------------------------------------------------------------------
# Configure CMake using the Visual Studio preset
# ---------------------------------------------------------------------------
Write-Host "`nConfiguring CMake with preset: windows-msvc-vs"

& cmake --preset windows-msvc-vs
if ($LASTEXITCODE -ne 0) {
    Fail "CMake configuration with preset 'windows-msvc-vs' failed."
}

Write-Host "`n=== Sentinel Windows Setup Complete ==="
Write-Host "Next steps:"
Write-Host "  1) Build Debug:"
Write-Host "       cmake --build --preset windows-msvc-vs --config Debug"
Write-Host "  2) Run the GUI:"
Write-Host "       build\\windows-msvc-vs\\apps\\sentinel_gui\\Debug\\sentinel_gui.exe"


