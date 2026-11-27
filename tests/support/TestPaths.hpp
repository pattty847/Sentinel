// Run-time paths for tests: the source tree, build scratch roots and helper executables.
//
// ctest passes them through each test's ENVIRONMENT property (see
// sentinel_test_paths() in the root CMakeLists.txt). They must never become
// compile definitions: an absolute path on a compile line differs per checkout,
// so every unit that carries it misses the shared ccache in each new worktree
// (ccache base_dir rewrites path arguments, not -D values).
//
// A test run by hand from the build directory exits with a message that names the
// variable to set, for example SENTINEL_SOURCE_DIR=<checkout>.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#ifdef QT_CORE_LIB
#include <QString>
#endif

namespace testpaths {

// $name followed by suffix. Exits the test process when the variable is unset.
inline std::string env(const char* name, std::string_view suffix = {}) {
    const char* value = std::getenv(name);
    if (!value || !*value) {
        std::fprintf(stderr,
                     "%s is not set. Run this test through ctest, or set %s to the path that the test's "
                     "ENVIRONMENT property in tests/**/CMakeLists.txt gives it.\n",
                     name, name);
        std::fflush(stderr);
        std::exit(2);
    }
    std::string path(value);
    path.append(suffix);
    return path;
}

// $SENTINEL_SOURCE_DIR (the checkout root), or a path relative to it.
inline std::string source(std::string_view relative = {}) {
    std::string path = env("SENTINEL_SOURCE_DIR");
    if (!relative.empty()) {
        path += '/';
        path.append(relative);
    }
    return path;
}

#ifdef QT_CORE_LIB
inline QString qenv(const char* name, std::string_view suffix = {}) {
    return QString::fromStdString(env(name, suffix));
}
inline QString qsource(std::string_view relative = {}) {
    return QString::fromStdString(source(relative));
}
#endif

} // namespace testpaths
