#pragma once

#include <filesystem>
#include <optional>

namespace copilot {

struct Npm_program
{
    std::filesystem::path path;
    bool native;
};

std::optional<Npm_program> npm_program(const std::filesystem::path& resolved_launcher);

} // namespace copilot
