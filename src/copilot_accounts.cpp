#include "copilot_accounts.h"

#include "copilot_launcher.h"
#include "copilot_npm.h"

#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/sha256.h"

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <memory>
#include <system_error>
#include <utility>

namespace copilot {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

std::string operating_system_home()
{
#ifdef _WIN32
    PWSTR buffer = nullptr;
    const auto status = SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &buffer);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(buffer, CoTaskMemFree);
    return SUCCEEDED(status) ? addon::path_to_utf8(fs::path(path.get())) : std::string();
#else
    const auto size = sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> buffer(size > 0 ? static_cast<std::size_t>(size) : 1024);
    passwd entry{};
    passwd* result = nullptr;
    int status;
    while ((status = getpwuid_r(getuid(), &entry, buffer.data(), buffer.size(), &result)) == ERANGE) {
        buffer.resize(buffer.size() * 2);
    }
    return status == 0 && result ? std::string(entry.pw_dir) : std::string();
#endif
}

enum class Launcher_type
{
    UNRECOGNIZED,
    MISSING,
    NATIVE,
    NPM_JAVASCRIPT,
};

struct Profile_lookup
{
    bool native_cli = false;
    const char* failure_message = "Logonomic could not identify the Copilot installation used by this launcher. "
        "Point a Direct launcher at the standalone GitHub Copilot CLI executable.";
};

Launcher_type launcher_type(
    const std::string& word, addon::Environment environment, std::string_view directory,
    [[maybe_unused]] std::string_view runner)
{
#ifdef _WIN32
    const bool cmd        = runner == addon::k_runner_cmd;
    const bool powershell = runner == addon::k_runner_powershell;
    if (cmd || powershell) {
        auto extension_entry = std::find_if(environment.begin(), environment.end(), [](const auto& entry) {
            return addon::lower(entry.first) == "pathext";
        });
        const auto extensions = extension_entry == environment.end() ? ".COM;.EXE;.BAT;.CMD" : extension_entry->second;
        addon::set_environment(environment, "PATHEXT", (powershell ? ".PS1;" : "") + extensions);
        if (cmd && std::none_of(environment.begin(), environment.end(), [](const auto& entry) {
                return addon::lower(entry.first) == "nodefaultcurrentdirectoryinexepath";
            }))
        {
            addon::set_environment(environment, "PATH",
                std::string(directory) + ";" + addon::environment_value(environment, "PATH"));
        }
        // Cmd and PowerShell also apply executable suffixes to a path with a
        // directory component. npm places an extensionless Unix shim beside them.
        const auto path = addon::path_from_utf8(word);
        if (path.has_parent_path() && !path.has_extension()) {
            addon::set_environment(environment, "PATH", addon::path_to_utf8(path.parent_path()));
            return launcher_type(addon::path_to_utf8(path.filename()), std::move(environment), directory, "");
        }
    }
#endif
    const auto program = addon::resolve_program(word, environment, directory);
    if (!program) {
        return Launcher_type::MISSING;
    }
    if (addon::native_image(*program)) {
        return Launcher_type::NATIVE;
    }
    const auto npm = npm_program(*program);
    if (!npm) {
        return Launcher_type::UNRECOGNIZED;
    }
    return npm->native ? Launcher_type::NATIVE : Launcher_type::NPM_JAVASCRIPT;
}

std::optional<fs::path> absolute_argument(
    std::string_view value, const fs::path& base, const fs::path& home, bool expand_home)
{
    auto path = addon::path_from_utf8(value);
    if (expand_home && !path.empty() && *path.begin() == "~") {
        if (!home.is_absolute()) {
            return std::nullopt;
        }
        path = home / path.lexically_relative("~");
    }
#ifdef _WIN32
    if (path.has_root_name() && !path.is_absolute() && path.root_name() != base.root_name()) {
        // A different drive's per-process working directory was not frozen.
        return std::nullopt;
    }
#else
    if (!path.is_absolute()) {
        // Copilot resolves an operand lexically from the physical process
        // cwd. A preceding -C can have entered a symbolic link.
        std::error_code error;
        const auto cwd = fs::canonical(base, error);
        if (error) {
            return std::nullopt;
        }
        path = cwd / path;
    }
#endif
    return addon::normalized_resume_directory(addon::path_to_utf8(base / path));
}

std::optional<fs::path> profile_path(
    const addon::Launcher_request& launcher, const addon::Launch_environment& launch,
    const std::optional<addon::Launcher_invocation>& invocation, std::string_view directory,
    std::string_view fallback, Profile_lookup& lookup)
{
    if (launch.platform_id != addon::host_platform_id() || !invocation || invocation->argv.empty() ||
        !launcher_invocation(launcher, launch.platform_id))
    {
        return std::nullopt;
    }
    const auto environment = addon::environment_pairs(launch.effective_environment);
    const auto base = addon::path_from_utf8(directory);
    if (!base.is_absolute()) {
        return std::nullopt;
    }
    const auto type = launcher_type(invocation->argv.front(), environment, directory, launcher.runner_key);
    if (type == Launcher_type::MISSING) {
        lookup.failure_message = "The saved launcher could not find Copilot CLI. Check the executable path in the launcher.";
        return std::nullopt;
    }
    if (type == Launcher_type::UNRECOGNIZED) {
        return std::nullopt;
    }
#ifdef _WIN32
    auto home = addon::environment_value(environment, "USERPROFILE");
#else
    auto home = addon::environment_value(environment, "HOME");
#endif
    if (home.empty()) {
        home = fallback.empty() ? operating_system_home() : std::string(fallback);
    }
    const auto native_home = addon::path_from_utf8(home);
    if (type == Launcher_type::NPM_JAVASCRIPT) {
        // This npm CLI locates its configuration under XDG_CONFIG_HOME or
        // Node's home. Shell-local assignments aren't in the frozen environment.
        const auto& args = invocation->argv;
        const auto end   = std::find(args.begin() + 1, args.end(), "--");
        const auto native_option = [](std::string_view arg) {
            return arg == "--config-dir" || arg.starts_with("--config-dir=") || arg.starts_with("-C");
        };
        if (!invocation->environment_names.empty()) {
            lookup.failure_message = "Move the inline environment assignments from the Copilot command "
                "to the launcher's environment settings so Logonomic can identify its profile.";
            return std::nullopt;
        }
        if (std::any_of(args.begin() + 1, end, native_option)) {
            lookup.failure_message = "Remove --config-dir or -C from the launcher. This npm Copilot CLI uses "
                "~/.copilot by default. To choose another location, set XDG_CONFIG_HOME to the parent of .copilot.";
            return std::nullopt;
        }
        lookup.failure_message = "Logonomic could not locate this npm Copilot CLI's configuration directory. "
            "Check the home directory and any XDG_CONFIG_HOME setting in the launcher environment.";
        const auto parent = addon::environment_value(environment, "XDG_CONFIG_HOME");
        if (!parent.empty()) {
            return absolute_argument(parent + "/.copilot", base, native_home, false);
        }
        return addon::normalized_resume_directory(addon::path_to_utf8(native_home / ".copilot"));
    }
    lookup.native_cli = true;
    lookup.failure_message = "Check the launcher's --config-dir and -C options. "
        "Use a full path for the Copilot configuration directory.";
    auto cwd = base;
    std::optional<std::string_view> config;
    bool changing_session_directory = false;
    const auto& args = invocation->argv;
    for (std::size_t i = 1; i < args.size(); ++i) {
        std::string_view arg(args[i]);
        if (arg == "--") {
            break;
        }
        if (arg == "--resume" || arg.starts_with("--resume=") || arg == "--continue" ||
            arg == "--worktree" || arg.starts_with("--worktree="))
        {
            changing_session_directory = true;
        }
        const bool is_config = arg == "--config-dir" || arg.starts_with("--config-dir=");
        const bool is_cwd = arg.starts_with("-C");
        if (!is_config && !is_cwd) {
            continue;
        }
        const auto name_size = is_config ? sizeof("--config-dir") - 1 : 2;
        auto value = arg.substr(name_size);
        if (value.empty()) {
            if (++i == args.size() || args[i].starts_with('-')) {
                return std::nullopt;
            }
            value = args[i];
        }
        else
        if (value.front() == '=') {
            value.remove_prefix(1);
        }
        if (value.empty() && !is_config) {
            return std::nullopt;
        }
        if (is_config) {
            config = value;
        }
        else {
            const auto changed = absolute_argument(value, base, native_home, true);
            if (!changed) {
                return std::nullopt;
            }
            cwd = *changed;
        }
    }
    const auto override_home = addon::environment_value(environment, "COPILOT_HOME");
    if (!config && override_home.empty()) {
        if (!native_home.is_absolute()) {
            return std::nullopt;
        }
        return addon::normalized_resume_directory(addon::path_to_utf8(native_home / ".copilot"));
    }
    const auto value = config.value_or(override_home);
    if (changing_session_directory && !addon::path_from_utf8(value).is_absolute()) {
        return std::nullopt;
    }
    return absolute_argument(value, cwd, native_home, config.has_value());
}

std::string profile_key(const fs::path& profile)
{
    const auto identity = addon::native_account_profile_identity(profile);
    return identity ? "profile-" + addon::to_hex(addon::Sha256::digest(*identity)) : std::string{};
}

addon::Account_status availability(const fs::path& profile)
{
    std::error_code error;
    const auto status = fs::status(profile_io_path(profile), error);
    if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) {
        return addon::Account_status::UNAVAILABLE;
    }
    if (error) {
        return addon::Account_status::UNKNOWN;
    }
    return fs::is_directory(status) ? addon::Account_status::AVAILABLE : addon::Account_status::UNAVAILABLE;
}

} // namespace

std::filesystem::path profile_io_path(const std::filesystem::path& profile)
{
#ifdef _WIN32
    const auto spelling = fs::path(profile).make_preferred().native();
    if (spelling.starts_with(L"\\\\?\\") || spelling.starts_with(L"\\\\.\\")) {
        return spelling;
    }
    return spelling.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + spelling.substr(2) : L"\\\\?\\" + spelling;
#else
    return profile;
#endif
}

void accounts(addon::Invocation_context& context)
{
    const auto& request = context.request();
    if (request.operation != addon::k_accounts_discover_operation && request.operation != addon::k_accounts_resolve_operation) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto input = addon::decode_accounts_launcher_request(request.payload, &failure);
    if (!input) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto invocation = launcher_invocation(input->launcher, input->launch_environment.platform_id);
    Profile_lookup lookup;
    const auto path = profile_path(input->launcher, input->launch_environment, invocation, input->normalization_directory,
        input->default_home_fallback, lookup);
    if (!path) {
        context.fail(addon::Result_status::UNAVAILABLE, "copilot_profile_unavailable", lookup.failure_message);
        return;
    }
    const auto text = addon::path_to_utf8(*path);
    if (text.size() > addon::limits::k_max_start_absolute_path_bytes) {
        context.fail(addon::Result_status::UNAVAILABLE, "copilot_profile_unavailable",
            "The Copilot profile path is too long for Logonomic. Choose a shorter profile path.");
        return;
    }
    const auto key = profile_key(*path);
    if (key.empty()) {
        context.fail(addon::Result_status::UNAVAILABLE, "copilot_profile_unavailable");
        return;
    }
    if (request.operation == addon::k_accounts_resolve_operation) {
        context.succeed(addon::encode_account_resolution({key}));
        return;
    }
    context.succeed(addon::encode_accounts({{key, addon::account_display_name(text), availability(*path), text}}));
}

std::optional<Source_profile> selected_profile(
    const std::optional<addon::Account_context>& account,
    const addon::Launcher_request& launcher, const addon::Launch_environment& environment,
    const std::optional<addon::Launcher_invocation>& invocation, std::string_view working_directory)
{
    Profile_lookup lookup;
    const auto path = profile_path(launcher, environment, invocation, working_directory, {}, lookup);
    if (!path) {
        return std::nullopt;
    }
    if (!account || (profile_key(*path) == account->adapter_account_key &&
        (account->version == 1 || (account->profile_path &&
            addon::same_native_account_profile(addon::path_from_utf8(*account->profile_path), *path)))))
    {
        return Source_profile{*path, lookup.native_cli};
    }
    return std::nullopt;
}

bool accept_account(
    addon::Invocation_context& context, const std::optional<addon::Account_context>& account,
    const addon::Launcher_request& launcher, const addon::Launch_environment& environment,
    const std::optional<addon::Launcher_invocation>& invocation, std::string_view working_directory)
{
    if (!account || selected_profile(account, launcher, environment, invocation, working_directory)) {
        return true;
    }
    context.fail(addon::Result_status::UNAVAILABLE, "account_profile_mismatch",
        "The selected Copilot profile does not match this launch.");
    return false;
}

} // namespace copilot
