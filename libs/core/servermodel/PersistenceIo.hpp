#pragma once
// Native persistence primitives shared by HMCL and HMC2. No format policy here.
#include <cerrno>
#include <filesystem>
#include <span>
#include <cstdint>
#include <algorithm>
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
#ifndef _WIN32
// Sync an already-owned descriptor, including the drive cache on Darwin. Some
// filesystems/descriptors do not support F_FULLFSYNC; retain the fsync fallback.
inline bool syncFileDescriptor(int fd, int &errorCode) {
    int result;
#ifdef __APPLE__
    do { result = ::fcntl(fd, F_FULLFSYNC); } while (result != 0 && errno == EINTR);
    if (result == 0) return true;
#endif
    do { result = ::fsync(fd); } while (result != 0 && errno == EINTR);
    if (result != 0) errorCode = errno;
    return result == 0;
}
#endif

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
    const bool ok = syncFileDescriptor(fd, errorCode);
    ::close(fd);
    return ok;
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
inline bool lockIsContended(int error) {
#ifdef _WIN32
    return error == ERROR_LOCK_VIOLATION || error == ERROR_SHARING_VIOLATION;
#else
    return error == EWOULDBLOCK || error == EAGAIN;
#endif
}
// Exclusive creation protects existing history even if generation selection is
// wrong or another actor creates the path between enumeration and this call.
inline bool writeNewFileExclusive(const std::filesystem::path &path, std::span<const uint8_t> bytes, int &error) {
#ifdef _WIN32
    HANDLE fd =
        CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fd == INVALID_HANDLE_VALUE) {
        error = static_cast<int>(GetLastError());
        return false;
    }
    bool ok = true;
    while (!bytes.empty()) {
        DWORD written = 0;
        if (!WriteFile(fd, bytes.data(), static_cast<DWORD>(std::min<size_t>(bytes.size(), MAXDWORD)), &written,
                       nullptr) ||
            !written) {
            error = static_cast<int>(GetLastError());
            ok = false;
            break;
        }
        bytes = bytes.subspan(written);
    }
    if (!CloseHandle(fd) && ok) {
        error = static_cast<int>(GetLastError());
        ok = false;
    }
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        error = errno;
        return false;
    }
    bool ok = true;
    while (!bytes.empty()) {
        const auto written = ::write(fd, bytes.data(), bytes.size());
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            error = written < 0 ? errno : EIO;
            ok = false;
            break;
        }
        bytes = bytes.subspan(static_cast<size_t>(written));
    }
    if (::close(fd) != 0 && ok) {
        error = errno;
        ok = false;
    }
#endif
    return ok;
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
    // Best effort on Windows: request a metadata flush, but a directory the user
    // may not open for write (a drive root, C:\Users) is not a failure. NTFS
    // journals directory metadata; file-data flushes (syncFile) stay mandatory.
    HANDLE fd =
        CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (fd == INVALID_HANDLE_VALUE) {
        error = static_cast<int>(GetLastError());
        return error == ERROR_ACCESS_DENIED;
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
