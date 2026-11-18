#include "copilot_initial_prompt.h"

#include "copilot_launcher.h"

#include "logonomic_cli_addon/launcher_utilities.h"
#include "logonomic_cli_addon/package_source.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace addon = logonomic::cli_addon;

namespace copilot {

namespace {

template<std::size_t Size>
bool contains(const std::string_view (&values)[Size], std::string_view value)
{
    return std::find(std::begin(values), std::end(values), value) != std::end(values);
}

bool fresh_interactive_arguments(const std::vector<std::string>& argv, const addon::Environment& environment)
{
    // These options have documented settings/permission semantics. Unknown
    // options cannot prove a fresh session: a future selector or command may
    // resume history or submit another message before the host's prompt.
    static constexpr std::string_view flags[]{
        "--allow-all", "--allow-all-tools", "--allow-all-paths", "--allow-all-urls",
        "--allow-all-mcp-server-instructions", "--assisted-approval", "--banner",
        "--disable-builtin-mcps", "--disable-client-telemetry", "--disallow-temp-dir",
        "--enable-all-github-mcp-tools", "--experimental", "--no-experimental", "--no-ask-user",
        "--no-auto-update", "--no-bash-env", "--no-color", "--no-custom-instructions",
        "--no-eager-powershell-resolution", "--no-mouse", "--screen-reader", "--yolo",
    };
    static constexpr std::string_view operands[]{
        "--model", "--context", "--reasoning-effort", "--name", "-n", "-C", "--config-dir",
        "--log-dir", "--log-level", "--add-dir", "--plugin-dir", "--additional-mcp-config",
        "--add-github-mcp-tool", "--add-github-mcp-toolset", "--disable-mcp-server",
        "--enable-mcp-server", "--mcp-github-auth", "--max-ai-credits", "--usage-output-file",
    };
    static constexpr std::string_view lists[]{
        "--allow-tool", "--deny-tool", "--allow-url", "--deny-url",
        "--available-tools", "--excluded-tools", "--secret-env-vars",
    };

    bool explicit_mode = false;
    for (std::size_t index = 1; index < argv.size(); ++index) {
        const std::string_view argument = argv[index];
        const auto equals = argument.find('=');
        const auto option = argument.substr(0, equals);
        if (contains(flags, option) && equals == std::string_view::npos) {
            continue;
        }
        if (contains(operands, option) || option == "--mode") {
            std::string_view value;
            if (equals != std::string_view::npos) {
                value = argument.substr(equals + 1);
            }
            else {
                if (++index == argv.size()) {
                    return false;
                }
                value = argv[index];
            }
            if (option == "--config-dir" && equals != std::string_view::npos) {
                continue;
            }
            if (value.empty() || value.front() == '-' || (option == "--mode" && value != "interactive")) {
                return false;
            }
            explicit_mode = explicit_mode || option == "--mode";
            continue;
        }
        if (contains(lists, option)) {
            if (equals != std::string_view::npos) {
                if (argument.substr(equals + 1).empty()) {
                    return false;
                }
            }
            else {
                while (index + 1 < argv.size() && !argv[index + 1].starts_with('-')) {
                    ++index;
                }
            }
            continue;
        }
        return false;
    }
    // Native explicit mode selection takes precedence over the environment.
    return explicit_mode || !plan_then_autopilot_requested(environment);
}

void unavailable(addon::Invocation_context& context, std::string detail)
{
    context.fail(addon::Result_status::UNAVAILABLE, addon::reason::k_initial_prompt_unavailable, std::move(detail));
}

} // namespace

void prepare_initial_prompt(
    const addon::Prepare_request& request, addon::Launch_patch patch, addon::Invocation_context& context)
{
    const auto& prompt = request.initial_prompt->text;
    static constexpr std::string_view shortcuts[]{"$", "?", "help", "quit", "exit", "exit print"};
    if (trim_native_whitespace(prompt) != prompt || prompt.starts_with('/') || prompt.starts_with('!') ||
        contains(shortcuts, addon::lower(prompt)))
    {
        unavailable(context, "Copilot input shortcuts and surrounding whitespace cannot carry a verbatim first message.");
        return;
    }
    const auto invocation = addon::literal_launcher_invocation(
        request.launcher, request.launch_environment->platform_id, request.launcher_argv);
    const auto environment = addon::environment_pairs(request.launch_environment->effective_environment);
    if (request.launcher.runner_key != addon::k_runner_direct || !invocation ||
        !launcher_invocation(request.launcher, request.launch_environment->platform_id) ||
        !fresh_interactive_arguments(invocation->argv, environment))
    {
        unavailable(context, "The selected invocation does not establish a fresh Direct Copilot session.");
        return;
    }

    const auto program = addon::resolve_program(invocation->argv.front(), environment, request.working_directory);
    if (!program || !addon::native_image(*program)) {
        unavailable(context, "A native Copilot executable is required for first-message delivery.");
        return;
    }

    auto argv = invocation->argv;
    argv.front() = addon::path_to_utf8(*program);
    // Freeze the binary whose features were probed; Copilot can otherwise
    // dispatch to an automatically downloaded executable before startup.
    if (std::find(argv.begin() + 1, argv.end(), "--no-auto-update") == argv.end()) {
        argv.emplace_back("--no-auto-update");
    }
    // An attached operand also carries a message beginning with '-' literally;
    // a separate operand can be parsed as another native CLI option.
    argv.push_back("--interactive=" + prompt);
    patch.argv = std::move(argv);
    patch.initial_prompt = addon::Initial_prompt_acknowledgment{addon::initial_prompt_text_digest_hex(prompt)};
    patch.disposition = addon::Launch_disposition{true, false, false};
    addon::Contract_failure failure;
    if (!addon::validate_launch_patch(patch, request.declared_assets, {}, &failure)) {
        unavailable(context, "The prepared first-message invocation exceeds the launch contract.");
        return;
    }

    const auto help = probe_help(addon::path_to_utf8(*program), environment, request.working_directory, context);
    if (context.cancel_requested()) {
        context.fail(addon::Result_status::CANCELLED, "cancelled");
        return;
    }
    if (!help || !help_declares(help->output, "--interactive") || !help_declares(help->output, "--no-auto-update")) {
        unavailable(context, "The installed Copilot did not establish native interactive-prompt support.");
        return;
    }
    context.succeed(addon::encode_launch_patch(patch));
}

} // namespace copilot
