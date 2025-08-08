#!/bin/bash

# Sentinel Cross-Platform Setup Script
# Automatically detects your platform and guides you through setup

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Detect operating system
detect_os() {
    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
        echo "linux"
    elif [[ "$OSTYPE" == "darwin"* ]]; then
        echo "macos"
    elif [[ "$OSTYPE" == "cygwin" ]] || [[ "$OSTYPE" == "msys" ]]; then
        echo "windows"
    else
        echo "unknown"
    fi
}

# Print colored output
print_status() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_header() {
    echo -e "${BLUE}$1${NC}"
}

# Check if command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Check system requirements
check_requirements() {
    print_header "🔍 Checking System Requirements..."
    
    local missing_tools=()
    
    # Check for git
    if ! command_exists git; then
        missing_tools+=("git")
    fi
    
    # Check for cmake
    if ! command_exists cmake; then
        missing_tools+=("cmake")
    else
        local cmake_version=$(cmake --version | head -n1 | sed 's/.*version //')
        local required_version="3.16"
        if ! printf '%s\n%s\n' "$required_version" "$cmake_version" | sort -V -C; then
            print_warning "CMake version $cmake_version found, but $required_version+ required"
        else
            print_status "CMake $cmake_version ✓"
        fi
    fi
    
    # Check for vcpkg
    if [ -n "$VCPKG_ROOT" ] && [ -d "$VCPKG_ROOT" ]; then
        print_status "vcpkg found at $VCPKG_ROOT ✓"
    else
        missing_tools+=("vcpkg")
    fi
    
    if [ ${#missing_tools[@]} -ne 0 ]; then
        print_error "Missing required tools: ${missing_tools[*]}"
        return 1
    fi
    
    print_status "All requirements met! ✓"
    return 0
}

# Install vcpkg
install_vcpkg() {
    print_header "📦 Installing vcpkg..."
    
    local vcpkg_dir
    local os=$(detect_os)
    
    if [ "$os" = "windows" ]; then
        vcpkg_dir="/c/vcpkg"
    else
        vcpkg_dir="$HOME/vcpkg"
    fi
    
    if [ -d "$vcpkg_dir" ]; then
        print_status "vcpkg already exists at $vcpkg_dir"
    else
        print_status "Cloning vcpkg to $vcpkg_dir..."
        git clone https://github.com/Microsoft/vcpkg.git "$vcpkg_dir"
    fi
    
    cd "$vcpkg_dir"
    
    if [ "$os" = "windows" ]; then
        if [ ! -f "./vcpkg.exe" ]; then
            print_status "Bootstrapping vcpkg..."
            ./bootstrap-vcpkg.bat
        fi
    else
        if [ ! -f "./vcpkg" ]; then
            print_status "Bootstrapping vcpkg..."
            ./bootstrap-vcpkg.sh
        fi
    fi
    
    # Set environment variable
    export VCPKG_ROOT="$vcpkg_dir"
    print_status "Set VCPKG_ROOT=$VCPKG_ROOT"
    
    # Add to shell profile
    local shell_profile=""
    if [ -n "$ZSH_VERSION" ]; then
        shell_profile="$HOME/.zshrc"
    elif [ -n "$BASH_VERSION" ]; then
        shell_profile="$HOME/.bashrc"
    fi
    
    if [ -n "$shell_profile" ]; then
        if ! grep -q "VCPKG_ROOT" "$shell_profile" 2>/dev/null; then
            echo "export VCPKG_ROOT=\"$vcpkg_dir\"" >> "$shell_profile"
            print_status "Added VCPKG_ROOT to $shell_profile"
        fi
    fi
}

# Install platform-specific dependencies
install_platform_deps() {
    local os=$(detect_os)
    
    print_header "🔧 Installing Platform Dependencies for $os..."
    
    case $os in
        "linux")
            if command_exists apt; then
                print_status "Installing build tools via apt..."
                sudo apt update
                sudo apt install -y build-essential cmake ninja-build git curl zip unzip tar pkg-config
                sudo apt install -y libgl1-mesa-dev libxkbcommon-dev libfontconfig1-dev
            elif command_exists dnf; then
                print_status "Installing build tools via dnf..."
                sudo dnf install -y gcc-c++ cmake ninja-build git curl zip unzip tar pkg-config
            elif command_exists pacman; then
                print_status "Installing build tools via pacman..."
                sudo pacman -S --needed base-devel cmake ninja git
            else
                print_warning "Unknown Linux distribution. Please install build tools manually."
            fi
            ;;
        "macos")
            print_status "Checking Xcode Command Line Tools..."
            if ! xcode-select -p >/dev/null 2>&1; then
                print_status "Installing Xcode Command Line Tools..."
                xcode-select --install
            fi
            
            if command_exists brew; then
                print_status "Installing additional tools via Homebrew..."
                brew install cmake ninja
            else
                print_warning "Homebrew not found. Consider installing it for easier dependency management."
            fi
            ;;
        "windows")
            print_warning "Windows detected. Please ensure you have Visual Studio 2019+ or MinGW installed."
            print_warning "Also install CMake and Git for Windows if not already available."
            ;;
        *)
            print_error "Unsupported operating system: $os"
            return 1
            ;;
    esac
}

# Build Sentinel
build_sentinel() {
    print_header "🏗️ Building Sentinel..."
    
    local os=$(detect_os)
    local preset="default"
    
    case $os in
        "linux") preset="linux-gcc" ;;
        "macos") preset="macos-clang" ;;
        "windows") preset="windows-msvc" ;;
    esac
    
    if [ ! -f "CMakeLists.txt" ]; then
        print_error "Not in a Sentinel project directory. Please cd to the project root."
        return 1
    fi
    
    print_status "Configuring with preset: $preset"
    cmake --preset "$preset"
    
    print_status "Building..."
    cmake --build "build-$preset" --config Release
    
    print_status "Running tests..."
    cd "build-$preset"
    ctest --output-on-failure --config Release
    cd ..
    
    print_status "Build complete! ✓"
    
    # Show run instructions
    print_header "🚀 How to Run Sentinel:"
    case $os in
        "windows")
            echo "  GUI: ./build-windows/apps/sentinel_gui/Release/sentinel_gui.exe"
            echo "  CLI: ./build-windows/apps/stream_cli/Release/stream_cli.exe"
            ;;
        *)
            echo "  GUI: ./build-$preset/apps/sentinel_gui/sentinel_gui"
            echo "  CLI: ./build-$preset/apps/stream_cli/stream_cli"
            ;;
    esac
}

# Main setup flow
main() {
    print_header "======================================"
    print_header "  Sentinel Cross-Platform Setup"
    print_header "======================================"
    
    local os=$(detect_os)
    print_status "Detected operating system: $os"
    
    # Check requirements first
    if ! check_requirements; then
        print_header "🛠️ Installing Missing Requirements..."
        
        # Install vcpkg if missing
        if [ -z "$VCPKG_ROOT" ] || [ ! -d "$VCPKG_ROOT" ]; then
            install_vcpkg
        fi
        
        # Install platform dependencies
        install_platform_deps
        
        print_status "Requirements installation complete!"
    fi
    
    # Ask if user wants to build now
    echo
    read -p "Build Sentinel now? (y/N): " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        build_sentinel
    else
        print_status "Skipping build. You can build later with:"
        print_status "  cmake --preset default && cmake --build build --config Release"
    fi
    
    print_header "✅ Setup Complete!"
    print_status "For detailed documentation, see: docs/CROSS_PLATFORM_BUILD.md"
}

# Run main function
main "$@" 