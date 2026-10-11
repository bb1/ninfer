#include "artifact/file_io.h"

#include "artifact/framing.h"
#include "artifact/schema.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ninfer::artifact {
namespace {

[[noreturn]] void fail(const std::filesystem::path& path, const char* operation) {
    throw ArtifactError(path.string() + ": " + operation + ": " + std::strerror(errno));
}

off_t file_offset(std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        throw ArtifactError("file offset exceeds positional I/O range");
    }
    return static_cast<off_t>(offset);
}

// Maximum single-read length: ssize_t on POSIX, DWORD for Windows ReadFile.
#ifdef _WIN32
constexpr std::size_t kMaxReadLength = std::numeric_limits<DWORD>::max();
#else
constexpr std::size_t kMaxReadLength = static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
#endif

#ifdef _WIN32

[[noreturn]] void win_fail(const std::filesystem::path& path, const char* operation, DWORD error) {
    char message[512] = {};
    ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                     message, static_cast<DWORD>(sizeof(message)), nullptr);
    auto end = message + std::strlen(message);
    while (end > message && (*(end - 1) == '\n' || *(end - 1) == '\r')) { --end; }
    *end = '\0';
    throw ArtifactError(path.string() + ": " + operation + ": " + message);
}

LARGE_INTEGER file_position(const std::filesystem::path& path, std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw ArtifactError(path.string() + ": file offset exceeds positional I/O range");
    }
    LARGE_INTEGER position {};
    position.QuadPart = static_cast<LONGLONG>(offset);
    return position;
}

HANDLE open_handle(const std::filesystem::path& path, DWORD flags) {
    // POSIX readers never block writers; grant share-write so in-place artifact updates work
    // while a Reader holds the file (matching Linux behavior).
    const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        win_fail(path, "open", ::GetLastError());
    }
    return handle;
}

std::size_t read_at(HANDLE handle, const std::filesystem::path& path, const char* operation,
                    std::uint64_t offset, std::span<std::byte> destination) {
    LARGE_INTEGER position = file_position(path, offset);
    DWORD received         = 0;
    if (!::SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) ||
        !::ReadFile(handle, destination.data(), static_cast<DWORD>(destination.size()), &received,
                    nullptr)) {
        win_fail(path, operation, ::GetLastError());
    }
    return received;
}

#endif // _WIN32

} // namespace

InputFile::InputFile(std::filesystem::path path) : path_(std::move(path)) {
#ifdef _WIN32
    handle_ = open_handle(path_, FILE_ATTRIBUTE_NORMAL);
    BY_HANDLE_FILE_INFORMATION info {};
    if (!::GetFileInformationByHandle(static_cast<HANDLE>(handle_), &info)) {
        const DWORD error = ::GetLastError();
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
        win_fail(path_, "fstat", error);
    }
    if (::GetFileType(static_cast<HANDLE>(handle_)) != FILE_TYPE_DISK ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
        throw ArtifactError(path_.string() + ": expected a regular file");
    }
    bytes_ = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
#else
    fd_ = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) { fail(path_, "open"); }

    struct stat status {};

    if (::fstat(fd_, &status) != 0) {
        const auto error = errno;
        ::close(fd_);
        fd_   = -1;
        errno = error;
        fail(path_, "fstat");
    }
    if (status.st_size < 0 || !S_ISREG(status.st_mode)) {
        ::close(fd_);
        fd_ = -1;
        throw ArtifactError(path_.string() + ": expected a regular file");
    }
    bytes_ = static_cast<std::uint64_t>(status.st_size);
#endif
}

InputFile::~InputFile() {
#ifdef _WIN32
    if (direct_handle_ != nullptr) { ::CloseHandle(static_cast<HANDLE>(direct_handle_)); }
    if (handle_ != nullptr) { ::CloseHandle(static_cast<HANDLE>(handle_)); }
#else
    if (direct_fd_ >= 0) { ::close(direct_fd_); }
    if (fd_ >= 0) { ::close(fd_); }
#endif
}

void InputFile::read_exact(std::uint64_t offset, std::span<std::byte> destination) const {
    if (offset > bytes_ || destination.size() > bytes_ - offset) {
        throw ArtifactError(path_.string() + ": read exceeds file length");
    }
#ifdef _WIN32
    while (!destination.empty()) {
        const auto count = std::min<std::size_t>(destination.size(), 64ULL * 1024 * 1024);
        const auto read  = read_at(static_cast<HANDLE>(handle_), path_, "pread", offset,
                                   destination.first(count));
        if (!read) { throw ArtifactError(path_.string() + ": unexpected EOF"); }
        offset += read;
        destination = destination.subspan(read);
    }
#else
    while (!destination.empty()) {
        const auto count = std::min<std::size_t>(destination.size(), 64ULL * 1024 * 1024);
        const auto read  = ::pread(fd_, destination.data(), count, file_offset(offset));
        if (read < 0) {
            if (errno == EINTR) { continue; }
            fail(path_, "pread");
        }
        if (!read) { throw ArtifactError(path_.string() + ": unexpected EOF"); }
        offset += static_cast<std::uint64_t>(read);
        destination = destination.subspan(static_cast<std::size_t>(read));
    }
#endif
}

std::size_t InputFile::read_direct(std::uint64_t offset, std::span<std::byte> destination) const {
    if (offset % kPayloadAlignment || destination.size() % kPayloadAlignment ||
        reinterpret_cast<std::uintptr_t>(destination.data()) % kPayloadAlignment ||
        destination.size() > kMaxReadLength) {
        throw ArtifactError(path_.string() + ": unaligned or oversized direct read");
    }
    if (destination.empty()) { return 0; }
#ifdef _WIN32
    // The 4096-byte alignment enforced above is a multiple of every Windows sector size,
    // satisfying FILE_FLAG_NO_BUFFERING.
    if (direct_handle_ == nullptr) {
        direct_handle_ = open_handle(path_, FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN);
    }
    LARGE_INTEGER position = file_position(path_, offset);
    DWORD received         = 0;
    if (!::SetFilePointerEx(static_cast<HANDLE>(direct_handle_), position, nullptr, FILE_BEGIN) ||
        !::ReadFile(static_cast<HANDLE>(direct_handle_), destination.data(),
                    static_cast<DWORD>(destination.size()), &received, nullptr)) {
        win_fail(path_, "direct pread", ::GetLastError());
    }
    return received;
#else
    if (direct_fd_ < 0) {
        direct_fd_ = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECT);
        if (direct_fd_ < 0) { fail(path_, "open direct"); }
    }
    ssize_t read;
    do {
        read = ::pread(direct_fd_, destination.data(), destination.size(), file_offset(offset));
    } while (read < 0 && errno == EINTR);
    if (read < 0) { fail(path_, "direct pread"); }
    return static_cast<std::size_t>(read);
#endif
}

} // namespace ninfer::artifact
