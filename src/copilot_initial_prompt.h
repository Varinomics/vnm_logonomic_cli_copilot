#pragma once

#include "logonomic_cli_addon/adapter_runtime.h"
#include "logonomic_cli_addon/launch_preparation.h"

namespace copilot {

void prepare_initial_prompt(
    const logonomic::cli_addon::Prepare_request& request,
    logonomic::cli_addon::Launch_patch patch,
    logonomic::cli_addon::Invocation_context& context);

} // namespace copilot
