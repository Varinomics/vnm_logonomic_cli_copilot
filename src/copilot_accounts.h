#pragma once

#include "logonomic_cli_addon/accounts.h"
#include "logonomic_cli_addon/adapter_runtime.h"
#include "logonomic_cli_addon/launcher_utilities.h"

namespace copilot {

std::filesystem::path profile_io_path(const std::filesystem::path& profile);

void accounts(logonomic::cli_addon::Invocation_context& context);

bool accept_account(
    logonomic::cli_addon::Invocation_context& context,
    const std::optional<logonomic::cli_addon::Account_context>& account,
    const logonomic::cli_addon::Launcher_request& launcher,
    const logonomic::cli_addon::Launch_environment& environment,
    const std::optional<logonomic::cli_addon::Launcher_invocation>& invocation,
    std::string_view working_directory);

} // namespace copilot
