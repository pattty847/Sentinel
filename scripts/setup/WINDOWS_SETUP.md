# Sentinel Windows Setup Guide

**Purpose**: Migrate Sentinel from WSL to native Windows for better graphics performance.

---

## Prerequisites (Install on Windows)

### 1. Git for Windows
- Download: https://git-scm.com/download/win
- Use default settings (Git Bash + Git from command line)

### 2. Visual Studio 2022 Community
- Download: https://visualstudio.microsoft.com/downloads/
- Required workloads during installation:
  - ✅ **Desktop development with C++**
  - ✅ **CMake tools for Windows** (included in C++ workload)
- Individual components to verify:
  - MSVC v143 or later (C++ compiler)
  - Windows 11 SDK (or Windows 10 SDK)
  - C++ CMake tools for Windows

### 3. vcpkg (C++ Package Manager)
```powershell
# Open PowerShell as Administrator
cd C:\
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg integrate install

# Add to PATH (or use full path later)
# System Properties → Environment Variables → System Variables → Path
# Add: C:\vcpkg
```

### 4. Qt 6 (via vcpkg)
Qt will be installed via vcpkg automatically when building, but you need to enable it:
```powershell
# In C:\vcpkg
.\vcpkg install qt6-base:x64-windows qt6-charts:x64-windows qt6-quick:x64-windows qt6-quickcontrols2:x64-windows qt6-svg:x64-windows
```
This takes 30-60 minutes on first install. Qt is large.

---

## Transfer Project from WSL

### Option A: Using Git (Recommended)

**On WSL:**
```bash
cd /home/pepe/projects/Sentinel

# Check current branch
git status
# You're on: claude/review-msdf-heatmap-Wem52

# Stage and commit current work
git add -A
git commit -m "Pre-Windows migration checkpoint

- MSDF label caching optimizations
- Config system (YAML)
- PerformanceMonitor with FPS tracking
- Debug mode documentation
"

# Push to remote (if you have one)
git push origin claude/review-msdf-heatmap-Wem52

# Or create a bundle if no remote
git bundle create sentinel-wsl.bundle --all
# Copy bundle to Windows: /mnt/c/Users/YourName/sentinel-wsl.bundle
```

**On Windows:**
```powershell
# Open PowerShell or Git Bash
cd C:\Users\YourName\projects

# If you pushed to remote:
git clone <your-repo-url> Sentinel
cd Sentinel
git checkout claude/review-msdf-heatmap-Wem52

# If using bundle:
git clone C:\Users\YourName\sentinel-wsl.bundle Sentinel
cd Sentinel
```

### Option B: Direct File Copy (If Git Issues)

**On WSL:**
```bash
cd /home/pepe/projects/Sentinel

# Create clean archive (excludes build, .git if wanted)
tar --exclude='build' \
    --exclude='.cursor' \
    --exclude='*.log' \
    --exclude='resources/glyph-cache' \
    --exclude='.cache' \
    --exclude='vcpkg_installed' \
    -czf /mnt/c/Users/YourName/sentinel-clean.tar.gz .
```

**On Windows:**
```powershell
cd C:\Users\YourName\projects
mkdir Sentinel
cd Sentinel
tar -xzf C:\Users\YourName\sentinel-clean.tar.gz
```

---

## Build Configuration on Windows

### 1. CMake Toolchain Setup

Create `CMakePresets.json` in project root (if not exists):
```json
{
  "version": 3,
  "configurePresets": [
    {
      "name": "windows-msvc",
      "displayName": "Windows MSVC",
      "description": "MSVC with vcpkg",
      "generator": "Visual Studio 17 2022",
      "architecture": {
        "value": "x64",
        "strategy": "set"
      },
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "RelWithDebInfo",
        "CMAKE_TOOLCHAIN_FILE": "C:/vcpkg/scripts/buildsystems/vcpkg.cmake",
        "VCPKG_TARGET_TRIPLET": "x64-windows"
      },
      "binaryDir": "${sourceDir}/build-windows"
    }
  ],
  "buildPresets": [
    {
      "name": "windows-msvc-release",
      "configurePreset": "windows-msvc",
      "configuration": "RelWithDebInfo"
    }
  ]
}
```

### 2. Install Dependencies via vcpkg

```powershell
cd C:\Users\YourName\projects\Sentinel

# Install dependencies from vcpkg.json
C:\vcpkg\vcpkg install --triplet x64-windows

# This will install:
# - Qt6 (base, charts, quick, quickcontrols2, svg, qml)
# - yaml-cpp
# - msdfgen
# - Boost (if needed)
```

**Expected time**: 30-90 minutes (Qt is huge, msdfgen is fast)

### 3. Configure with CMake

**Using Visual Studio:**
1. Open Visual Studio 2022
2. File → Open → CMake... → Select `CMakeLists.txt`
3. VS will auto-detect CMakePresets.json
4. Select configuration: `windows-msvc`
5. Wait for CMake configure to complete

**Using Command Line:**
```powershell
cd C:\Users\YourName\projects\Sentinel

# Configure
cmake --preset windows-msvc

# Build
cmake --build build-windows --config RelWithDebInfo -j8
```

### 4. Build Project

**In Visual Studio:**
- Build → Build All (Ctrl+Shift+B)
- Or right-click `sentinel_gui` → Build

**Command Line:**
```powershell
cmake --build build-windows --config RelWithDebInfo --target sentinel_gui -j8
```

Output will be in: `build-windows\apps\sentinel_gui\RelWithDebInfo\sentinel_gui.exe`

---

## Running on Windows

### Set Environment Variables (First Run)

**Option 1: PowerShell (Temporary)**
```powershell
$env:SENTINEL_HEATMAP_TF = "1s"
$env:SENTINEL_SERVER_HOST = "127.0.0.1"
$env:SENTINEL_SERVER_PORT = "17000"

.\build-windows\apps\sentinel_gui\RelWithDebInfo\sentinel_gui.exe
```

**Option 2: Use sentinel.yaml (Preferred)**
```yaml
# sentinel.yaml in project root
heatmap:
  timeframe: "1s"
  grid_height: 5120
  # ... rest of config

server:
  host: "127.0.0.1"
  port: 17000
  # ... rest

gui:
  screenshot_dir: "./screenshots"
  api_port: 17100
```

Then just run:
```powershell
.\build-windows\apps\sentinel_gui\RelWithDebInfo\sentinel_gui.exe
```

### Run Server + GUI

**Terminal 1 (Server):**
```powershell
.\build-windows\apps\sentinel-server\RelWithDebInfo\sentinel-server.exe
```

**Terminal 2 (GUI):**
```powershell
.\build-windows\apps\sentinel_gui\RelWithDebInfo\sentinel_gui.exe
```

---

## Troubleshooting

### Qt DLLs Not Found
```
The code execution cannot proceed because Qt6Core.dll was not found
```

**Fix**: Add Qt bin directory to PATH or copy DLLs
```powershell
# Option 1: Add to PATH
$env:Path += ";C:\vcpkg\installed\x64-windows\bin"

# Option 2: Use windeployqt (after build)
C:\vcpkg\installed\x64-windows\tools\qt6\windeployqt.exe .\build-windows\apps\sentinel_gui\RelWithDebInfo\sentinel_gui.exe
```

### vcpkg Toolchain Not Found
```
CMake Error: Could not find CMAKE_TOOLCHAIN_FILE
```

**Fix**: Update path in CMakePresets.json to match your vcpkg location
- Default: `C:/vcpkg/scripts/buildsystems/vcpkg.cmake`
- If you installed elsewhere, adjust accordingly

### MSVC Compiler Not Found
```
CMake Error: No CMAKE_CXX_COMPILER could be found
```

**Fix**: Install Visual Studio C++ workload or run from "Developer PowerShell for VS 2022"
- Start Menu → Visual Studio 2022 → Developer PowerShell for VS 2022

### Long Path Issues
```
MSB3491: Could not write lines to file "x64\Release\sentinel.tlog\..."
```

**Fix**: Enable long paths on Windows
```powershell
# Run as Administrator
New-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem" -Name "LongPathsEnabled" -Value 1 -PropertyType DWORD -Force
```

Restart after enabling.

### Qt Scene Graph Validation Errors
```
QSGBatchRenderer: Failed to create shader
```

**Fix**: Ensure you're using native Windows graphics drivers (not WSL forwarding)
- Check Device Manager → Display Adapters
- Update GPU drivers from manufacturer (NVIDIA/AMD/Intel)

---

## Performance Validation

After successful build and run:

### 1. Check FPS
Status bar should now show **60 FPS** consistently (native vsync)

### 2. Test Interactions
- Hover toolbar icons → Should be instant, no lag
- Pan/zoom viewport → Smooth 60fps
- Auto-scroll → No stuttering

### 3. Run Debug Mode (Optional)
```powershell
# Clear old WSL logs
rm .cursor\debug.log

# Run with instrumentation
.\build-windows\apps\sentinel_gui\RelWithDebInfo\sentinel_gui.exe

# Interact for 30 seconds, then check log
type .cursor\debug.log
```

Expected: Far fewer `frame_swap_slow` (H5) events, much lower `durationMs` values (<16ms)

---

## Next Steps After Migration

1. **Update .gitignore** (if not already):
   ```gitignore
   build-windows/
   vcpkg_installed/
   .vs/
   *.user
   ```

2. **Commit Windows-specific changes**:
   ```powershell
   git add CMakePresets.json docs/WINDOWS_SETUP.md
   git commit -m "Add Windows build configuration"
   ```

3. **Remove WSL artifacts** (if switching permanently):
   ```powershell
   # On WSL, clean up
   rm -rf /home/pepe/projects/Sentinel/build
   rm .cursor/debug.log
   ```

4. **Benchmark before/after**:
   - WSL: 50-63 FPS, 40-400ms frame swaps, laggy input
   - Windows: 60 FPS, <16ms frame swaps, instant input ✅

---

## Development Workflow on Windows

### Visual Studio (Recommended)
- Open `CMakeLists.txt` as CMake project
- Edit code in VS editor
- F5 to build + debug
- Integrated debugger with breakpoints

### VS Code (Alternative)
- Install extensions: C/C++, CMake Tools
- Open folder in VS Code
- Select kit: "Visual Studio Community 2022 Release - amd64"
- CMake: Configure → Build → Debug

### Command Line (Power Users)
```powershell
# Rebuild after changes
cmake --build build-windows --config RelWithDebInfo -j8

# Clean build
cmake --build build-windows --target clean
cmake --build build-windows --config RelWithDebInfo -j8

# Run
.\build-windows\apps\sentinel_gui\RelWithDebInfo\sentinel_gui.exe
```

---

**Estimated total setup time**: 2-3 hours (mostly waiting for Qt to compile)

**Expected performance gain**: 10-20x reduction in frame swap latency, 60fps locked, instant input response
