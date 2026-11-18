#pragma once

#include <filesystem>
#include <functional>
#include <string_view>

namespace copilot {

// Reads only complete lines of the captured append-only native journal prefix.
void read_usage_journal(
    const std::filesystem::path& profile, const std::string& session,
    const std::function<void(std::string_view)>& consume, const std::function<void()>& check_progress);

} // namespace copilot
