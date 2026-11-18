#pragma once

#include <filesystem>
#include <optional>

namespace copilot {

std::optional<std::filesystem::path> npm_native_program(const std::filesystem::path& resolved_launcher);

} // namespace copilot
