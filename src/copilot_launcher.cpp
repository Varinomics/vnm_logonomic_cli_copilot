#include "copilot_launcher.h"

#include "copilot_initial_prompt.h"

#include "logonomic_cli_addon/launch_preparation.h"

#include <string>
#include <utility>
#include <vector>

namespace addon = logonomic::cli_addon;

namespace copilot {

namespace {

void reject(addon::Invocation_context& context, const addon::Contract_failure& failure)
{
    context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
}

bool copilot_program(std::string_view program, std::string_view platform)
{
    std::string name = addon::base_name(program);
    if (platform.starts_with("windows-")) {
        name = addon::lower(std::move(name));
        return name == "copilot"     || name == "copilot.exe" ||
               name == "copilot.cmd" || name == "copilot.bat" || name == "copilot.ps1";
    }
    return name == "copilot";
}

} // namespace

std::optional<addon::Launcher_invocation> launcher_invocation(
    const addon::Launcher_request& launcher, std::string_view platform)
{
    auto invocation = addon::literal_launcher_invocation(launcher, platform);
    if (!invocation || invocation->argv.empty() || !copilot_program(invocation->argv.front(), platform)) {
        return std::nullopt;
    }
    return invocation;
}

void discover(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_discover_request(context.request().payload, &failure);
    if (!request) {
        reject(context, failure);
        return;
    }

    const auto environment = addon::environment_pairs(request->ambient_environment);
    const auto program = addon::resolve_program("copilot", environment, request->default_home_fallback);
    std::vector<addon::Launcher_candidate> candidates;
    if (program) {
        addon::Launcher_candidate candidate;
        candidate.candidate_id = "copilot";
        candidate.display_name = "GitHub Copilot CLI";
        candidate.script_body  = "copilot";
        candidate.runner_key = addon::preferred_discovery_runner(
            *program, request->platform_id.starts_with("windows-") ? addon::k_runner_cmd : addon::k_runner_bash);
        candidate.is_default = true;
        candidate.evidence.set("source", addon::Json_value("path"));
        candidates.push_back(std::move(candidate));
    }
    context.succeed(addon::encode_candidates(candidates));
}

void recognize(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_recognize_request(context.request().payload, &failure);
    if (!request) {
        reject(context, failure);
        return;
    }

    addon::Launcher_recognition recognition;
    recognition.recognized = launcher_invocation(request->launcher, request->platform_id).has_value();
    if (recognition.recognized) {
        recognition.display_name = "GitHub Copilot CLI";
        recognition.evidence.set("source", addon::Json_value("literal-invocation"));
    }
    context.succeed(addon::encode_recognition(recognition));
}

void prepare(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_prepare_request(context.request().payload, &failure);
    if (!request) {
        reject(context, failure);
        return;
    }

    const auto invocation = addon::literal_launcher_invocation(
        request->launcher, request->launch_environment->platform_id, request->launcher_argv);
    addon::Launch_patch patch;
    patch.profile_storage = request->profile_storage;
    patch.account_context = request->account_context;
    if (request->initial_prompt) {
        prepare_initial_prompt(*request, std::move(patch), context);
        return;
    }
    patch.disposition = addon::Launch_disposition{
        invocation && !invocation->argv.empty() &&
            copilot_program(invocation->argv.front(), request->launch_environment->platform_id),
        false,
        false,
    };
    // No invocation rewrite is necessary: the host preserves the chosen shell,
    // arguments and effective environment, including Copilot's permission policy.
    context.succeed(addon::encode_launch_patch(patch));
}

} // namespace copilot
