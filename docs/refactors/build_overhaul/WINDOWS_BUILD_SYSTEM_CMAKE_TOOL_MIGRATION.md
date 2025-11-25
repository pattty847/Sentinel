**Title:** *Windows Build System Overhaul + CMake Modernization + Toolchain Migration*

**Summary:**
This PR overhauls the Windows build toolchain, removes stale MinGW dependencies, migrates the project to MSVC/vcpkg, fixes Qt6 detection issues, and applies multiple structural upgrades to the CMake infrastructure. These changes significantly stabilize Windows builds, unify cross-platform behavior, and prepare for the ongoing dataflow refactor.

---

### **1. Toolchain Migration**

* Removed all MinGW/GCC dependencies.
* Cleaned PATH pollution from Cursor and MSYS2.
* Fully migrated Windows builds to **MSVC 2022 (BuildTools)**.
* Added `/bigobj` for Boost/Beast compatibility.
* Defined `_WIN32_WINNT=0x0A00` to target Windows 10 APIs.

**Result:** Windows builds now work reliably using the correct compiler.

---

### **2. Qt6 Fixes**

* Removed stale hardcoded MSYS2 Qt paths.
* Installed Qt6.9.3 (msvc2022_64).
* Added correct CMake detection path via `CMAKE_PREFIX_PATH`.
* Ensured correct component loading (`Charts`, `Quick`, `Widgets`, etc.).
* Replaced all `Qt::` prefixes with **`Qt6::`** for compatibility and clarity.

---

### **3. Dependency Management Cleanup**

* Removed `pkg_check_modules()` and all PkgConfig usage on Windows.
* Standardized dependency resolution through **vcpkg**:

  * `openssl`
  * `boost`
  * `nlohmann-json`
  * `jwt-cpp`
* Deleted redundant `find_package()` calls in apps/ that duplicated library-level discovery.

**Result:** No more mixed toolchain dependencies; everything is clean and consistent.

---

### **4. CMake Modernization**

* Upgraded root `cmake_minimum_required` to **3.22**.
* Removed conflicting `cmake_minimum_required()` calls inside subdirectories.
* Added MSVC-specific compile options (`/bigobj`, Win10 API).
* Removed obsolete MinGW prefixes.
* Centralized platform detection.
* Fixed incorrect or missing Qt6 policies.
* Ensured consistent FetchContent usage.

---

### **5. Cross-Platform Presets (Prep Work)**

* Reworked Windows preset to use MSVC + vcpkg toolchain.
* Added support for environment-variable based paths:

  * `QT_MSVC`
  * `VCPKG_ROOT`
* Cleaned up presets to avoid absolute paths.

**Result:** Presets are now portable and consistent across OSes.

---

### **6. Project Structure and Dataflow Refactor Safety**

* Retained all functional changes from Phase 1 of the Dataflow Refactor.
* No behavioral changes introduced in this PR.
* Rebuild after toolchain migration is clean and stable.