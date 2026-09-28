#pragma once
// Native persistence primitives shared by HMCL and HMC2. No format policy here.
#include <cerrno>
#include <filesystem>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace sentinel::persistence {
inline bool syncFilePath(const std::filesystem::path &path, int &errorCode) {
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        errorCode = static_cast<int>(GetLastError());
        return false;
    }
    const bool ok = FlushFileBuffers(file) != 0;
    if (!ok) {
        errorCode = static_cast<int>(GetLastError());
    }
    CloseHandle(file);
    return ok;
#else
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        errorCode = errno;
        return false;
    }
#ifdef __APPLE__
    int result = ::fcntl(fd, F_FULLFSYNC);
    if (result != 0) {
        result = ::fsync(fd);
    }
#else
    const int result = ::fsync(fd);
#endif
    if (result != 0) {
        errorCode = errno;
    }
    ::close(fd);
    return result == 0;
#endif
}

#ifdef _WIN32
using LockHandle = HANDLE;
inline constexpr LockHandle noLock = nullptr;
#else
using LockHandle = int;
inline constexpr LockHandle noLock = -1;
#endif
inline LockHandle acquireFileLock(const std::filesystem::path &path, int &error) {
#ifdef _WIN32
    HANDLE fd = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fd == INVALID_HANDLE_VALUE) {
        error = static_cast<int>(GetLastError());
        return noLock;
    }
    OVERLAPPED ov{};
    if (!LockFileEx(fd, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &ov)) {
        error = static_cast<int>(GetLastError());
        CloseHandle(fd);
        return noLock;
    }
#else
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        error = errno;
        return noLock;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        error = errno;
        ::close(fd);
        return noLock;
    }
#endif
    return fd;
}
inline void releaseFileLock(LockHandle fd) {
    if (fd == noLock)
        return;
#ifdef _WIN32
    OVERLAPPED ov{};
    UnlockFileEx(fd, 0, MAXDWORD, MAXDWORD, &ov);
    CloseHandle(fd);
#else
    ::close(fd);
#endif
}
inline bool syncDirectory(const std::filesystem::path &path, int &error) {
#ifdef _WIN32
    // Request a real metadata flush. Some Windows filesystems deny directory
    // flushes; surface that failure instead of claiming unprovided durability.
    HANDLE fd =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (fd == INVALID_HANDLE_VALUE) {
        error = static_cast<int>(GetLastError());
        return false;
    }
    const bool ok = FlushFileBuffers(fd) != 0;
    if (!ok)
        error = static_cast<int>(GetLastError());
    CloseHandle(fd);
    return ok;
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        error = errno;
        return false;
    }
    const int result = ::fsync(fd);
    if (result != 0)
        error = errno;
    ::close(fd);
    return result == 0;
#endif
}
} // namespace sentinel::persistence
