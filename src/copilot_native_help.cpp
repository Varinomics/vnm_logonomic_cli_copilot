#include "copilot_native_help.h"

#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/sha256.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include <algorithm>
#include <chrono>
#include <mutex>

namespace copilot {
namespace {

namespace addon = logonomic::cli_addon;

bool help_declares(std::string_view help, std::string_view option)
{
    while (!help.empty()) {
        const auto end = help.find('\n');
        const auto line = addon::trim(help.substr(0, end), addon::Launcher_whitespace::ASCII);
        const auto position = line.find(option);
        if (line.starts_with('-') && position != std::string::npos &&
            (position == 0 || line[position - 1] == ' ' || line[position - 1] == ',') &&
            (position + option.size() == line.size() || line[position + option.size()] == ' '))
        {
            return true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        help.remove_prefix(end + 1);
    }
    return false;
}

std::optional<addon::Sha256_digest> probe_identity(
    const std::string& program, const addon::Environment& environment, std::string_view working_directory)
{
    addon::Sha256 hash;
#ifdef _WIN32
    const auto path = addon::path_from_utf8(program);
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    FILE_BASIC_INFO basic{};
    FILE_ID_INFO id{};
    const bool identified = GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) &&
        GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id));
    CloseHandle(file);
    if (!identified) {
        return std::nullopt;
    }
    hash.update(&id.VolumeSerialNumber, sizeof(id.VolumeSerialNumber));
    hash.update(id.FileId.Identifier, sizeof(id.FileId.Identifier));
    hash.update(&basic.ChangeTime.QuadPart, sizeof(basic.ChangeTime.QuadPart));
#else
    struct stat info{};
    if (stat(program.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        return std::nullopt;
    }
    hash.update(&info.st_dev, sizeof(info.st_dev));
    hash.update(&info.st_ino, sizeof(info.st_ino));
#ifdef __APPLE__
    const auto changed = info.st_ctimespec;
#else
    const auto changed = info.st_ctim;
#endif
    hash.update(&changed.tv_sec, sizeof(changed.tv_sec));
    hash.update(&changed.tv_nsec, sizeof(changed.tv_nsec));
#endif
    // Contract strings cannot contain NUL. Hash the complete frozen scope
    // without retaining launcher secrets or native help/diagnostic text.
    const auto field = [&](std::string_view value) {
        hash.update(value);
        hash.update("\0", 1);
    };
    field(program);
    field(working_directory);
    for (const auto& [name, value] : environment) {
        field(name);
        field(value);
    }
    return hash.finish();
}

struct Probe_cache
{
    std::mutex mutex;
    std::optional<addon::Sha256_digest> identity;
    std::chrono::steady_clock::time_point expires;
    Native_help options{};
};

} // namespace

std::optional<Native_help> native_help(
    const std::string& program, const addon::Environment& environment,
    std::string_view working_directory, addon::Invocation_context& context)
{
    static Probe_cache cache;
    const auto before = probe_identity(program, environment, working_directory);
    if (before) {
        const std::lock_guard lock(cache.mutex);
        if (before == cache.identity && std::chrono::steady_clock::now() < cache.expires) {
            return cache.options;
        }
    }
    // First startup extracts the installed runtime before it can print help.
    // Honour a shorter host deadline and bound callers that omit one.
    constexpr std::int64_t maximum_ms = 45000;
    const auto timeout = std::min<std::int64_t>(context.request().deadline_ms, maximum_ms);
    const auto help = addon::probe({program, "--no-auto-update", "--help"}, environment,
        working_directory, static_cast<int>(timeout), context);
    if (!help || context.cancel_requested()) {
        return std::nullopt;
    }
    const auto declares = [&](std::string_view option) { return help_declares(help->output, option); };
    const bool fixed_binary = declares("--no-auto-update");
    const bool structured = fixed_binary && declares("--session-id") && declares("--output-format") && declares("--stream");
    const Native_help options{fixed_binary && declares("--interactive"), structured,
        structured && declares("--additional-mcp-config") && declares("--allow-tool"),
        declares("--model"), declares("--reasoning-effort")};
    if (before) {
        if (before != probe_identity(program, environment, working_directory)) {
            return std::nullopt;
        }
        const std::lock_guard lock(cache.mutex);
        cache.identity = before;
        cache.expires = std::chrono::steady_clock::now() + std::chrono::minutes(1);
        cache.options = options;
    }
    return options;
}

} // namespace copilot
