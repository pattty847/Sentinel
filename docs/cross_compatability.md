# **Sentinel Cross-Platform Build Guide (2025 Update)**

Sentinel now uses a **unified, modern, zero-friction build system** across Windows, macOS, and Linux:

* **CMakePresets.json** handles all configuration
* Qt is provided via **platform-specific env vars**
* Dependencies come from **vcpkg** only
* Compiler toolchains are **not installed by scripts**
* Bootstrap scripts simply validate your environment

This doc explains exactly how to get Sentinel building on each platform with the new architecture.

---

# 🚀 Core Philosophy

**1. You install Qt manually (installer or package manager).**
**2. You install a compiler manually (MSVC/Xcode/GCC).**
**3. You set 1–2 environment variables.**
**4. You run the CMake preset.**

Nothing else.
The tooling is deterministic and identical across OSes.

---

# 📦 Required Environment Variables

Sentinel uses three platform-specific Qt variables and one shared vcpkg variable:

## **Shared**

```
VCPKG_ROOT=/path/to/vcpkg
```

## **Windows**

```
QT_MSVC=C:/Qt/6.9.3/msvc2022_64
```

## **macOS**

```
QT_MAC=/opt/homebrew/opt/qt
```

## **Linux**

```
QT_LINUX=/usr/lib/qt6          # or your distro’s Qt6 path
```

You can export these inside your shell profile:

**PowerShell**

```powershell
setx VCPKG_ROOT "C:\dev\vcpkg"
setx QT_MSVC "C:\Qt\6.9.3\msvc2022_64"
```

**macOS/Linux**

```bash
echo 'export VCPKG_ROOT="$HOME/vcpkg"' >> ~/.zshrc
echo 'export QT_MAC="/opt/homebrew/opt/qt"' >> ~/.zshrc
```

---

# 🧱 Platform Prerequisites

## **Windows**

* Visual Studio 2022 Build Tools (MSVC)
* Qt 6.9+ (MSVC build)
* Git
* CMake (newest)
* Ninja (optional but recommended)

## **macOS**

* Xcode (for Clang toolchain)
* Qt 6.9+ via Homebrew (`brew install qt`)
* Git
* CMake
* Ninja

## **Linux**

* GCC 11+ or Clang 14+
* Qt 6.9+ (distro or Qt installer)
* Git
* CMake
* Ninja
* vcpkg bootstrapped from GitHub

---

# ✨ Building Sentinel (All Platforms)

Once environment variables are set, building Sentinel is identical everywhere:

### **Configure**

```
cmake --preset windows-msvc      # on Windows
cmake --preset mac-clang        # on macOS
cmake --preset linux-gcc        # on Linux
```

### **Build**

```
cmake --build --preset windows-msvc
```

### **Run**

Executables appear under:

```
build-<preset>/apps/
```

---

# 📁 Directory Layout (Important)

CMake builds go into platform-specific directories:

```
build/windows-msvc/
build/mac-clang/
build/linux-gcc/
```

Each is fully isolated — no pollution, no compiler mixing.

---

# 🔧 Bootstrap Scripts (Optional Helpers)

Sentinel ships two ultra-simple bootstrap scripts:

```
scripts/bootstrap-windows.ps1
scripts/bootstrap-unix.sh
```

They only:

1. Check that required env vars exist
2. Suggest fixes if missing
3. Print the correct preset to run

They do *not* install compilers, Qt, vcpkg, or system dependencies.

This prevents:

* accidental toolchain overrides
* MinGW/MSYS2 contamination
* weird PATH collisions
* Qt mismatches across machines

---

# 🧪 Optional: Running Tests

All tests run through **ctest** after the build:

```
cd build-<preset>
ctest --output-on-failure
```

---

# 🛠 IDE Notes

All major C++ IDEs auto-detect presets:

**VSCode (CMake Tools)**

* Automatically detects `CMakePresets.json`
* Uses compile commands generated per preset
* No special settings required

**CLion**

* Import project → presets auto-populate

**Qt Creator**

* Supports presets since Qt 6.5

---

# 🏁 Summary

Sentinel’s build system is now:

**Cross-platform, reproducible, deterministic, preset-driven.**

To build:

1. Install Qt + compiler
2. Set env vars
3. `cmake --preset <platform>`
4. `cmake --build --preset <platform>`

That’s it.