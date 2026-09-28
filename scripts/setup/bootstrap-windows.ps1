# Sentinel Bootstrap (Windows)
# Minimal setup: verify env vars and guide the dev

Write-Host "=== Sentinel Windows Bootstrap ===`n"

if (-not $env:VCPKG_ROOT) {
    Write-Warning "VCPKG_ROOT is not set."
    Write-Host "Set it using:"
    Write-Host '  setx VCPKG_ROOT "C:\dev\vcpkg"'
    exit 1
}

if (-not $env:QT_MSVC) {
    Write-Warning "QT_MSVC is not set."
    Write-Host "Set it to your Qt installation path, e.g.:"
    Write-Host '  setx QT_MSVC "C:\Qt\6.9.3\msvc2022_64"'
    exit 1
}

Write-Host "Environment looks good."
Write-Host "Run the following to configure:"
Write-Host "  cmake --preset windows-msvc"
Write-Host "Then build with:"
Write-Host "  cmake --build --preset windows-msvc"
