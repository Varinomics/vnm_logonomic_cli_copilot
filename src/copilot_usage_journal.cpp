#include "copilot_usage_journal.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace copilot {
namespace {
namespace fs = std::filesystem;
using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;

struct Captured_file
{
    File stream{nullptr, &std::fclose};
    std::uint64_t bytes = 0;
};

#ifdef _WIN32
struct Close_handle
{
    void operator()(void* value) const { CloseHandle(value); }
};
using Handle = std::unique_ptr<void, Close_handle>;

Handle open_native(const fs::path& path, bool directory, bool optional)
{
    const auto handle = CreateFileW(path.c_str(), directory ? FILE_READ_ATTRIBUTES : GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | (directory ? 0 : FILE_SHARE_DELETE), nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_FLAG_SEQUENTIAL_SCAN), nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto status = GetLastError();
        if (optional && (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)) return {};
        throw std::runtime_error("Copilot journal open failed");
    }
    Handle owned(handle);
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information) || GetFileType(handle) != FILE_TYPE_DISK ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory)
    {
        throw std::runtime_error("Copilot journal path changed");
    }
    return owned;
}

Captured_file capture(const fs::path& profile, const std::string& session)
{
    // Holding the directory handles without delete sharing prevents a path
    // component being replaced before its child is opened. Native writers can
    // continue appending or rotating the captured regular file.
    const auto history = profile / "session-state";
    auto parent = open_native(history, true, false);
    auto child = open_native(history / session, true, false);
    auto handle = open_native(history / session / "events.jsonl", false, true);
    if (!handle) return {};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle.get(), &size) || size.QuadPart < 0) throw std::runtime_error("Copilot journal size unavailable");
    const auto fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle.get()), _O_BINARY | _O_RDONLY);
    if (fd == -1) throw std::runtime_error("Copilot journal descriptor unavailable");
    handle.release();
    File file(_fdopen(fd, "rb"), &std::fclose);
    if (!file) {
        _close(fd);
        throw std::runtime_error("Copilot journal stream unavailable");
    }
    return {std::move(file), static_cast<std::uint64_t>(size.QuadPart)};
}
#else
struct Descriptor
{
    int value;
    ~Descriptor() { if (value >= 0) close(value); }
};

Captured_file capture(const fs::path& profile, const std::string& session)
{
    // The account owns its profile root; every component below it is opened
    // relative to the captured parent, rejecting links and special files.
    Descriptor root{open(profile.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (root.value < 0) throw std::runtime_error("Copilot profile unavailable");
    constexpr auto flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
    Descriptor history{openat(root.value, "session-state", flags)};
    if (history.value < 0) throw std::runtime_error("Copilot history unavailable");
    Descriptor child{openat(history.value, session.c_str(), flags)};
    if (child.value < 0) throw std::runtime_error("Copilot session unavailable");
    Descriptor descriptor{openat(child.value, "events.jsonl", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    if (descriptor.value < 0) {
        if (errno == ENOENT) return {};
        throw std::runtime_error("Copilot journal unavailable");
    }
    struct stat status{};
    if (fstat(descriptor.value, &status) || !S_ISREG(status.st_mode) || status.st_size < 0) {
        throw std::runtime_error("Copilot journal is not a regular file");
    }
    File file(fdopen(descriptor.value, "rb"), &std::fclose);
    if (!file) throw std::runtime_error("Copilot journal stream unavailable");
    descriptor.value = -1;
    return {std::move(file), static_cast<std::uint64_t>(status.st_size)};
}
#endif
} // namespace

void read_usage_journal(
    const fs::path& profile, const std::string& session, const std::function<void(std::string_view)>& consume,
    const std::function<void()>& check_progress)
{
    const auto captured = capture(profile, session);
    std::array<char, 64 * 1024> block;
    std::string line;
    constexpr std::size_t max_line = 64 * 1024 * 1024;
    for (auto remaining = captured.bytes; remaining > 0;) {
        check_progress();
        const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, block.size()));
        if (std::fread(block.data(), 1, wanted, captured.stream.get()) != wanted) {
            throw std::runtime_error("Copilot journal was truncated during capture");
        }
        remaining -= wanted;
        std::string_view part(block.data(), wanted);
        while (!part.empty()) {
            const auto end = part.find('\n');
            const auto prefix = part.substr(0, end);
            if (prefix.size() > max_line - line.size()) throw std::runtime_error("Copilot journal entry exceeds bounds");
            line.append(prefix);
            if (end == std::string_view::npos) break;
            consume(line);
            line.clear();
            part.remove_prefix(end + 1);
        }
    }
}

} // namespace copilot
