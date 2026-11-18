#include "copilot_npm_native.h"

#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/launcher_utilities.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/sha256.h"

#include <string>
#include <string_view>
#include <system_error>

namespace copilot {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs = std::filesystem;

constexpr std::string_view k_native_loader_digest = "d83678c00d0560eb715628b49318f6fc77e634696743aee025496956015f88e0";

std::optional<addon::Json_value> manifest(const fs::path& directory)
{
    const auto bytes = addon::Directory_package_source(directory).read("package.json", 64 * 1024, nullptr);
    return bytes ? addon::parse_json(*bytes, {}, nullptr) : std::nullopt;
}

std::string member(const addon::Json_value* value, std::string_view key)
{
    const auto* item = value && value->is_object() ? value->find(key) : nullptr;
    return item && item->is_string() ? item->as_string() : std::string{};
}

std::optional<fs::path> native_dependency(
    const fs::path& package, const addon::Json_value& wrapper, std::string_view platform)
{
#if defined(__aarch64__) || defined(_M_ARM64)
    constexpr auto arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    constexpr auto arch = "x64";
#else
    constexpr auto arch = "";
#endif
#ifdef _WIN32
    constexpr auto binary = "copilot.exe";
#else
    constexpr auto binary = "copilot";
#endif
    const auto name = "copilot-" + std::string(platform) + "-" + arch;
    const auto scoped_name = "@github/" + name;
    const auto required = member(wrapper.find("optionalDependencies"), scoped_name);
    if (required.empty() || required != member(&wrapper, "version")) {
        return std::nullopt;
    }
    // Node checks nested dependencies before the hoisted node_modules.
    for (const fs::path& root : {package / "node_modules", package.parent_path() / "node_modules",
                                package.parent_path().parent_path()}) {
        const auto directory = root / "@github" / name;
        std::error_code error;
        if (!fs::exists(directory, error) && !error) {
            continue;
        }
        const auto dependency = manifest(directory);
        if (!dependency || member(&*dependency, "name") != scoped_name || member(&*dependency, "version") != required ||
            member(dependency->find("exports"), ".") != std::string("./") + binary)
        {
            return std::nullopt;
        }
        const auto exported = directory / binary;
        const auto executable = addon::resolve_program(addon::path_to_utf8(exported), {}, {});
        // npm can replace this export on upgrade. Verify its target, but
        // keep the installed path and command name in the saved launcher.
        return executable && addon::native_image(*executable) ? std::optional(exported) : std::nullopt;
    }
    return std::nullopt;
}

} // namespace

std::optional<std::filesystem::path> npm_native_program(const std::filesystem::path& resolved_launcher)
{
#ifdef _WIN32
    const auto entry = addon::npm_node_shim_entry(resolved_launcher);
    if (!entry) {
        return std::nullopt;
    }
    const auto& loader = *entry;
#else
    std::error_code error;
    const auto loader = fs::canonical(resolved_launcher, error);
    if (error) {
        return std::nullopt;
    }
#endif
    const auto package = loader.parent_path();
    for (const auto& component : package) {
        // A saved Direct launcher must survive ordinary npm package updates.
        // Other package managers can put a version in the store path.
        if (component == ".pnpm" || component == ".store") {
            return std::nullopt;
        }
    }
    if (loader.filename() != "npm-loader.js" || package.filename() != "copilot" ||
        package.parent_path().filename() != "@github" || package.parent_path().parent_path().filename() != "node_modules")
    {
        return std::nullopt;
    }
    // The known npm loader delegates unchanged argv to the exported native
    // dependency. A custom or changed loader retains its normal shell launch.
    const auto code = addon::Directory_package_source(package).read("npm-loader.js", 16 * 1024, nullptr);
    if (!code || addon::to_hex(addon::Sha256::digest(*code)) != k_native_loader_digest) {
        return std::nullopt;
    }
    const auto wrapper = manifest(package);
    if (!wrapper || member(&*wrapper, "name") != "@github/copilot" ||
        member(wrapper->find("bin"), "copilot") != "npm-loader.js")
    {
        return std::nullopt;
    }
#if defined(_WIN32)
    return native_dependency(package, *wrapper, "win32");
#elif defined(__APPLE__)
    return native_dependency(package, *wrapper, "darwin");
#elif defined(__linux__)
#ifndef __GLIBC__
    if (auto executable = native_dependency(package, *wrapper, "linuxmusl")) {
        return executable;
    }
#endif
    return native_dependency(package, *wrapper, "linux");
#else
    return std::nullopt;
#endif
}

} // namespace copilot
