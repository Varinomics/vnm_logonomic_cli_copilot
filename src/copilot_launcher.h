#pragma once

#include "logonomic_cli_addon/adapter_runtime.h"
#include "logonomic_cli_addon/launcher_discovery.h"
#include "logonomic_cli_addon/launcher_utilities.h"

#include <optional>
#include <string_view>

namespace copilot {

std::optional<logonomic::cli_addon::Launcher_invocation> launcher_invocation(
    const logonomic::cli_addon::Launcher_request& launcher,
    std::string_view platform);

void discover(logonomic::cli_addon::Invocation_context& context);
void recognize(logonomic::cli_addon::Invocation_context& context);
void prepare(logonomic::cli_addon::Invocation_context& context);

} // namespace copilot
