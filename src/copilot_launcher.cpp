#include "copilot_launcher.h"

#include "copilot_accounts.h"
#include "copilot_initial_prompt.h"
#include "copilot_npm_native.h"

#include "logonomic_cli_addon/launch_preparation.h"
#include "logonomic_cli_addon/package_source.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
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

std::string_view trim_native_whitespace(std::string_view value)
{
    // Copilot trims ECMAScript whitespace, which includes BOM but excludes
    // NEL. The SDK's launcher-whitespace grammar has different boundaries.
    static constexpr std::string_view whitespace[]{
        "\t", "\n", "\v", "\f", "\r", " ", "\u00A0", "\u1680",
        "\u2000", "\u2001", "\u2002", "\u2003", "\u2004", "\u2005",
        "\u2006", "\u2007", "\u2008", "\u2009", "\u200A", "\u2028",
        "\u2029", "\u202F", "\u205F", "\u3000", "\uFEFF",
    };
    for (;;) {
        const auto previous_size = value.size();
        for (const auto space : whitespace) {
            if (value.starts_with(space)) {
                value.remove_prefix(space.size());
            }
            if (value.ends_with(space)) {
                value.remove_suffix(space.size());
            }
        }
        if (value.size() == previous_size) {
            return value;
        }
    }
}

bool plan_then_autopilot_requested(const addon::Environment& environment)
{
    const auto stored = addon::environment_value(environment, "COPILOT_PLAN_THEN_AUTOPILOT");
    const auto normalized = addon::lower(std::string(trim_native_whitespace(stored)));
    return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
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
        if (candidate.runner_key != addon::k_runner_direct) {
            if (const auto native = npm_native_program(*program)) {
                const auto path = addon::path_to_utf8(*native);
                // This text is saved as a Direct launcher and must round-trip
                // to exactly one path. Keep the shell launcher otherwise.
                const auto script = "\"" + path + "\"";
                if (script.size() <= addon::limits::k_max_script_body_bytes && path.find_first_of("\"\r\n") == path.npos &&
                    addon::literal_argv(script) == std::vector<std::string>{path})
                {
                    candidate.script_body = script;
                    candidate.runner_key = addon::k_runner_direct;
                    candidate.evidence.set("source", addon::Json_value("npm-native-package"));
                }
            }
        }
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
    if (!accept_account(context, request->account_context, request->launcher, *request->launch_environment,
            invocation, request->working_directory))
    {
        return;
    }
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
