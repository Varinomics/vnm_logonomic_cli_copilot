#pragma once

#include "logonomic_cli_addon/adapter_runtime.h"

namespace copilot {

void prepare_structured(logonomic::cli_addon::Invocation_context& context);
void prepare_delegated(logonomic::cli_addon::Invocation_context& context);
void inspect_session(logonomic::cli_addon::Invocation_context& context);

} // namespace copilot
