#include "copilot_accounts.h"
#include "copilot_usage.h"
#include "copilot_launcher.h"
#include "copilot_session.h"

#include "logonomic_cli_addon/adapter_runtime.h"
#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/launch_preparation.h"
#include "logonomic_cli_addon/launcher_discovery.h"
#include "logonomic_cli_addon/session_contracts.h"

#include <utility>

namespace {

namespace addon = logonomic::cli_addon;

void invoke(addon::Invocation_context& context)
{
    const auto& request = context.request();
    if (request.capability.capability_id == addon::k_profile_storage_capability_id &&
        request.operation == addon::k_profile_storage_acknowledge)
    {
        addon::acknowledge_profile_storage(context);
        return;
    }
    if (request.capability.capability_id == addon::k_usage_scan_capability_id) {
        copilot::scan_usage(context);
        return;
    }
    if (request.capability.capability_id == addon::k_accounts_capability_id) {
        copilot::accounts(context);
        return;
    }
    if (request.capability.capability_id == addon::k_launcher_discovery_capability_id) {
        if (request.operation == addon::k_discover_operation) {
            copilot::discover(context);
        }
        else
        if (request.operation == addon::k_recognize_operation) {
            copilot::recognize(context);
        }
        else {
            context.reject_unknown_operation();
        }
        return;
    }
    if (request.capability.capability_id == addon::k_launch_preparation_capability_id &&
        request.operation == addon::k_prepare_operation)
    {
        copilot::prepare(context);
        return;
    }
    if (request.capability.capability_id == addon::k_structured_session_capability_id) {
        if (request.operation == addon::k_prepare_operation) {
            copilot::prepare_structured(context);
        }
        else
        if (request.operation == addon::k_inspect_operation) {
            copilot::inspect_session(context);
        }
        else {
            context.reject_unknown_operation();
        }
        return;
    }
    if (request.capability.capability_id == addon::k_delegated_session_capability_id &&
        request.operation == addon::k_prepare_operation)
    {
        copilot::prepare_delegated(context);
        return;
    }
    context.reject_unknown_operation();
}

} // namespace

int main()
{
    addon::Adapter_runtime_options options;
    options.adapter_name    = "copilot-addon";
    options.adapter_version = "1.0.0";
    options.capabilities = {
        {addon::k_presentation_capability_id, addon::k_presentation_version},
        {addon::k_accounts_capability_id, addon::k_accounts_launcher_version},
        {addon::k_profile_storage_capability_id, addon::k_profile_storage_version},
        {addon::k_usage_scan_capability_id, addon::k_usage_scan_acceptance_version},
        {"usage-cross-source-event", 1},
        {addon::k_launcher_discovery_capability_id, addon::k_launcher_discovery_version},
        {addon::k_launch_preparation_capability_id, addon::k_launch_preparation_execution_version},
        {addon::k_interactive_initial_prompt_capability_id, addon::k_interactive_initial_prompt_version},
        {addon::k_structured_session_capability_id, addon::k_structured_session_version},
        {addon::k_delegated_session_capability_id, addon::k_delegated_session_version},
        {addon::k_delegated_model_selection_capability_id, addon::k_delegated_model_selection_version},
        {"structured-model-observation", addon::k_model_state_version},
    };
    addon::Adapter_runtime runtime(std::move(options));
    runtime.set_invoke_handler(invoke);
    return runtime.run();
}
