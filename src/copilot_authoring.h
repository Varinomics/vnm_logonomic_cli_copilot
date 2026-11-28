#pragma once

#include "logonomic_cli_addon/adapter_runtime.h"
#include "logonomic_cli_addon/launcher_utilities.h"
#include "logonomic_cli_addon/session_contracts.h"

namespace copilot {

bool supports_authoring(const std::string& program, const logonomic::cli_addon::Environment& environment,
    std::string_view working_directory, logonomic::cli_addon::Invocation_context& context);
bool configure_authoring(logonomic::cli_addon::Structured_prepare_result& result,
    const logonomic::cli_addon::Structured_prepare_request& request,
    const std::vector<std::string>& launcher_arguments, std::string session_id);
void inspect_authoring(logonomic::cli_addon::Invocation_context& context);

} // namespace copilot
