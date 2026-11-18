#pragma once

#include "logonomic_cli_addon/launcher_utilities.h"

namespace copilot {

struct Native_help
{
    bool first_prompt;
    bool structured;
    bool delegated;
    bool model;
    bool reasoning_effort;
};

std::optional<Native_help> native_help(
    const std::string& program, const logonomic::cli_addon::Environment& environment,
    std::string_view working_directory, logonomic::cli_addon::Invocation_context& context);

} // namespace copilot
