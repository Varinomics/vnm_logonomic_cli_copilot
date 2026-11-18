#pragma once

#include "logonomic_cli_addon/adapter_runtime.h"
#include "logonomic_cli_addon/launcher_discovery.h"
#include "logonomic_cli_addon/launcher_utilities.h"

#include <optional>
#include <string>
#include <string_view>

namespace copilot {

std::optional<logonomic::cli_addon::Launcher_invocation> launcher_invocation(
    const logonomic::cli_addon::Launcher_request& launcher,
    std::string_view platform);

bool help_declares(std::string_view help, std::string_view option);

std::optional<logonomic::cli_addon::Probe_output> probe_help(
    const std::string& program, const logonomic::cli_addon::Environment& environment,
    std::string_view working_directory, logonomic::cli_addon::Invocation_context& context);

std::string_view trim_native_whitespace(std::string_view value);

bool plan_then_autopilot_requested(const logonomic::cli_addon::Environment& environment);

void discover(logonomic::cli_addon::Invocation_context& context);
void recognize(logonomic::cli_addon::Invocation_context& context);
void prepare(logonomic::cli_addon::Invocation_context& context);

} // namespace copilot
