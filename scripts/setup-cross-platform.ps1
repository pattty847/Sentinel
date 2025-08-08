# Sentinel Cross-Platform Setup Script (PowerShell)
# Automatically detects Windows environment and guides through setup

param(
    [switch]$SkipBuild,
    [switch]$Help
)

# Function definitions
function Write-Status {
    param([string]$Message)
    Write-Host "[INFO] $Message" -ForegroundColor Green
}

function Write-Warning {
    param([string]$Message)
    Write-Host "[WARN] $Message" -ForegroundColor Yellow
}

function Write-Error {
    param([string]$Message)
    Write-Host "[ERROR] $Message" -ForegroundColor Red
}

function Write-Header {
    param([string]$Message)
    Write-Host $Message -ForegroundColor Blue
}

function Test-CommandExists {
    param([string]$Command)
    $null = Get-Command $Command -ErrorAction SilentlyContinue
    return $?
}

function Show-Help {
    Write-Header "======================================"
    Write-Header "  Sentinel Cross-Platform Setup"
    Write-Header "======================================"
    Write-Host ""
    Write-Host "Usage: .\setup-cross-platform.ps1 [options]"
    Write-Host ""
    Write-Host "Options:"
    Write-Host "  -SkipBuild    Skip the build step"
    Write-Host "  -Help         Show this help message"
    Write-Host ""
    Write-Host "This script will:"
    Write-Host "  1. Check system requirements"
    Write-Host "  2. Install vcpkg if needed"
    Write-Host "  3. Install required dependencies"
    Write-Host "  4. Build Sentinel (unless -SkipBuild)"
    Write-Host ""
    exit 0
}

function Test-Requirements {
    Write-Header "🔍 Checking System Requirements..."
    
    $missingTools = @()
    
    # Check for git
    if (-not (Test-CommandExists "git")) {
        $missingTools += "git"
    }
    
    # Check for cmake
    if (-not (Test-CommandExists "cmake")) {
        $missingTools += "cmake"
    } else {
        $cmakeVersion = (cmake --version | Select-Object -First 1) -replace '.*version\s+', ''
        $requiredVersion = [Version]"3.16"
        $currentVersion = [Version]($cmakeVersion -split ' ')[0]
        
        if ($currentVersion -lt $requiredVersion) {
            Write-Warning "CMake version $cmakeVersion found, but $requiredVersion+ required"
        } else {
            Write-Status "CMake $cmakeVersion ✓"
        }
    }
    
    # Check for vcpkg
    if ($env:VCPKG_ROOT -and (Test-Path $env:VCPKG_ROOT)) {
        Write-Status "vcpkg found at $env:VCPKG_ROOT ✓"
    } else {
        $missingTools += "vcpkg"
    }
    
    # Check for Visual Studio or Build Tools
    $vsInstalled = $false
    $vsPaths = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\*\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2019\*\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\*\MSBuild\Current\Bin\MSBuild.exe"
    )
    
    foreach ($path in $vsPaths) {
        if (Get-ChildItem $path -ErrorAction SilentlyContinue) {
            $vsInstalled = $true
            break
        }
    }
    
    if (-not $vsInstalled) {
        Write-Warning "Visual Studio 2019+ or Build Tools not found"
        $missingTools += "visual-studio"
    } else {
        Write-Status "Visual Studio Build Tools ✓"
    }
    
    if ($missingTools.Count -gt 0) {
        Write-Error "Missing required tools: $($missingTools -join ', ')"
        return $false
    }
    
    Write-Status "All requirements met! ✓"
    return $true
}

function Install-Vcpkg {
    Write-Header "📦 Installing vcpkg..."
    
    $vcpkgDir = "C:\vcpkg"
    
    if (Test-Path $vcpkgDir) {
        Write-Status "vcpkg already exists at $vcpkgDir"
    } else {
        Write-Status "Cloning vcpkg to $vcpkgDir..."
        git clone https://github.com/Microsoft/vcpkg.git $vcpkgDir
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Failed to clone vcpkg"
            exit 1
        }
    }
    
    Set-Location $vcpkgDir
    
    if (-not (Test-Path ".\vcpkg.exe")) {
        Write-Status "Bootstrapping vcpkg..."
        .\bootstrap-vcpkg.bat
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Failed to bootstrap vcpkg"
            exit 1
        }
    }
    
    # Set environment variable for current session
    $env:VCPKG_ROOT = $vcpkgDir
    Write-Status "Set VCPKG_ROOT=$env:VCPKG_ROOT"
    
    # Set environment variable permanently
    [Environment]::SetEnvironmentVariable("VCPKG_ROOT", $vcpkgDir, "User")
    Write-Status "Added VCPKG_ROOT to user environment variables"
}

function Install-Dependencies {
    Write-Header "🔧 Installing Windows Dependencies..."
    
    # Check for Chocolatey
    if (Test-CommandExists "choco") {
        Write-Status "Chocolatey found, installing tools..."
        choco install cmake ninja git -y
    } else {
        Write-Warning "Chocolatey not found. Please install manually:"
        Write-Host "  - CMake: https://cmake.org/download/"
        Write-Host "  - Git: https://git-scm.com/download/win"
        Write-Host "  - Ninja: https://ninja-build.org/"
    }
    
    # Check for Visual Studio
    $vsInstalled = $false
    $vsPaths = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\*\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2019\*\MSBuild\Current\Bin\MSBuild.exe"
    )
    
    foreach ($path in $vsPaths) {
        if (Get-ChildItem $path -ErrorAction SilentlyContinue) {
            $vsInstalled = $true
            break
        }
    }
    
    if (-not $vsInstalled) {
        Write-Warning "Visual Studio not detected. Please install:"
        Write-Host "  - Visual Studio 2019+ with C++ Desktop Development workload"
        Write-Host "  - Or Visual Studio Build Tools with C++ tools"
        Write-Host "  - Download from: https://visualstudio.microsoft.com/"
    }
}

function Build-Sentinel {
    Write-Header "🏗️ Building Sentinel..."
    
    if (-not (Test-Path "CMakeLists.txt")) {
        Write-Error "Not in a Sentinel project directory. Please cd to the project root."
        return $false
    }
    
    Write-Status "Configuring with preset: windows-msvc"
    cmake --preset windows-msvc
    if ($LASTEXITCODE -ne 0) {
        Write-Error "CMake configuration failed"
        return $false
    }
    
    Write-Status "Building..."
    cmake --build build-windows --config Release
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Build failed"
        return $false
    }
    
    Write-Status "Running tests..."
    Set-Location build-windows
    ctest --output-on-failure --config Release
    $testResult = $LASTEXITCODE
    Set-Location ..
    
    if ($testResult -ne 0) {
        Write-Warning "Some tests failed, but build completed"
    }
    
    Write-Status "Build complete! ✓"
    
    # Show run instructions
    Write-Header "🚀 How to Run Sentinel:"
    Write-Host "  GUI: .\build-windows\apps\sentinel_gui\Release\sentinel_gui.exe"
    Write-Host "  CLI: .\build-windows\apps\stream_cli\Release\stream_cli.exe"
    
    return $true
}

# Main execution
function Main {
    if ($Help) {
        Show-Help
    }
    
    Write-Header "======================================"
    Write-Header "  Sentinel Cross-Platform Setup"
    Write-Header "======================================"
    
    Write-Status "Detected operating system: Windows"
    
    # Check requirements first
    if (-not (Test-Requirements)) {
        Write-Header "🛠️ Installing Missing Requirements..."
        
        # Install vcpkg if missing
        if (-not $env:VCPKG_ROOT -or -not (Test-Path $env:VCPKG_ROOT)) {
            Install-Vcpkg
        }
        
        # Install platform dependencies
        Install-Dependencies
        
        Write-Status "Requirements installation complete!"
    }
    
    # Ask if user wants to build now
    if (-not $SkipBuild) {
        Write-Host ""
        $build = Read-Host "Build Sentinel now? (y/N)"
        if ($build -match "^[Yy]") {
            if (Build-Sentinel) {
                Write-Header "✅ Setup and Build Complete!"
            } else {
                Write-Error "Build failed. Check the output above for details."
            }
        } else {
            Write-Status "Skipping build. You can build later with:"
            Write-Status "  cmake --preset windows-msvc"
            Write-Status "  cmake --build build-windows --config Release"
        }
    } else {
        Write-Status "Skipping build (--SkipBuild specified)"
    }
    
    Write-Header "✅ Setup Complete!"
    Write-Status "For detailed documentation, see: docs\CROSS_PLATFORM_BUILD.md"
}

# Execute main function
Main 