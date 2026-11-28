#include "copilot_session.h"

#include "copilot_accounts.h"
#include "copilot_authoring.h"
#include "copilot_launcher.h"
#include "copilot_native_help.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/package_source.h"
#include "logonomic_cli_addon/session_contracts.h"
#include "logonomic_cli_addon/sha256.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace addon = logonomic::cli_addon;

namespace copilot {

namespace {

constexpr auto k_stream_limit = addon::limits::k_max_structured_stream_bytes;
constexpr std::size_t k_native_tool_name_limit = 64;
constexpr std::size_t k_identity_limit = 4096;

struct Server_state
{
    std::string name;
    std::int64_t tool_count = 0;
    bool connected = false;
    bool tools_loaded = false;
};

struct Session_state
{
    std::string session_id;
    std::string prompt_digest;
    std::string prompt_id;
    std::string tail;
    std::string answer;
    std::string response_id;
    std::string last_message_id;
    std::string chunk_text;
    std::int64_t chunk_count = 0;
    std::int64_t next_chunk = 0;
    bool started = false;
    bool answer_ready = false;
    bool result_seen = false;
    bool response_has_tools = false;
    std::vector<Server_state> servers;
};

struct Prepared_session
{
    addon::Process_proposal process;
    Session_state state;
    std::string program;
    addon::Environment environment;
};

template<std::size_t Size>
bool contains(const std::string_view (&values)[Size], std::string_view value)
{
    return std::find(std::begin(values), std::end(values), value) != std::end(values);
}

const std::string* string_member(const addon::Json_value& value, std::string_view name)
{
    const auto* member = value.find(name);
    return member && member->is_string() ? &member->as_string() : nullptr;
}

std::optional<std::int64_t> integer_member(const addon::Json_value& value, std::string_view name)
{
    const auto* member = value.find(name);
    return member && member->is_integer() ? std::optional(member->as_integer()) : std::nullopt;
}

std::optional<bool> boolean_member(const addon::Json_value& value, std::string_view name)
{
    const auto* member = value.find(name);
    return member && member->is_boolean() ? std::optional(member->as_boolean()) : std::nullopt;
}

bool identity(const std::string* value)
{
    return value && !value->empty() && value->size() <= k_identity_limit && value->find('\0') == std::string::npos;
}

std::string digest(std::string_view text)
{
    return addon::to_hex(addon::Sha256::digest(text));
}

void reject(addon::Invocation_context& context, const addon::Contract_failure& failure)
{
    context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
}

void unavailable(addon::Invocation_context& context, std::string detail)
{
    context.fail(addon::Result_status::UNAVAILABLE, addon::reason::k_launch_patch_invalid, std::move(detail));
}

bool implementation(addon::Structured_purpose purpose, addon::Invocation_context& context)
{
    if (purpose == addon::Structured_purpose::IMPLEMENTATION) {
        return true;
    }
    context.fail(addon::Result_status::UNAVAILABLE, addon::reason::k_purpose_unavailable);
    return false;
}

bool fresh_session_arguments(const std::vector<std::string>& argv, const addon::Environment& environment)
{
    // Programmatic execution must own the prompt, session and output channel.
    // Keep documented saved settings, but refuse unknown selectors that could
    // submit another prompt, change the workspace or hide injected task tools.
    static constexpr std::string_view flags[]{
        "--allow-all", "--allow-all-tools", "--allow-all-paths", "--allow-all-urls",
        "--allow-all-mcp-server-instructions", "--assisted-approval", "--banner",
        "--disable-builtin-mcps", "--disable-client-telemetry", "--disallow-temp-dir",
        "--enable-all-github-mcp-tools", "--experimental", "--no-experimental", "--no-ask-user",
        "--no-auto-update", "--no-bash-env", "--no-color", "--no-custom-instructions",
        "--no-eager-powershell-resolution", "--no-mouse", "--screen-reader", "--yolo",
    };
    static constexpr std::string_view operands[]{
        "--model", "--context", "--reasoning-effort", "--name", "-n", "--config-dir", "--log-dir", "--log-level",
        "--add-dir", "--plugin-dir", "--additional-mcp-config", "--add-github-mcp-tool",
        "--add-github-mcp-toolset", "--mcp-github-auth", "--max-ai-credits", "--usage-output-file",
    };
    static constexpr std::string_view lists[]{
        "--allow-tool", "--deny-tool", "--allow-url", "--deny-url", "--secret-env-vars",
    };
    bool explicit_interactive_mode = false;
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
            explicit_interactive_mode |= option == "--mode";
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
    return explicit_interactive_mode || !plan_then_autopilot_requested(environment);
}

std::string fresh_uuid()
{
    auto nonce = addon::random_nonce_hex();
    nonce[12] = '4';
    nonce[16] = "89ab"[(nonce[16] <= '9' ? nonce[16] - '0' : nonce[16] - 'a' + 10) & 3];
    return nonce.substr(0, 8) + "-" + nonce.substr(8, 4) + "-" + nonce.substr(12, 4) + "-" +
           nonce.substr(16, 4) + "-" + nonce.substr(20);
}

std::string encode_state(const Session_state& state)
{
    auto value = addon::Json_value::object();
    value.set("version", addon::Json_value(1));
    value.set("session_id", addon::Json_value(state.session_id));
    value.set("prompt_digest", addon::Json_value(state.prompt_digest));
    value.set("prompt_id", addon::Json_value(state.prompt_id));
    // A process read can split a UTF-8 sequence anywhere, including the tail.
    value.set("tail_base64", addon::Json_value(addon::encode_base64(state.tail)));
    value.set("answer", addon::Json_value(state.answer));
    value.set("response_id", addon::Json_value(state.response_id));
    value.set("last_message_id", addon::Json_value(state.last_message_id));
    value.set("chunk_text", addon::Json_value(state.chunk_text));
    value.set("chunk_count", addon::Json_value(state.chunk_count));
    value.set("next_chunk", addon::Json_value(state.next_chunk));
    value.set("started", addon::Json_value(state.started));
    value.set("answer_ready", addon::Json_value(state.answer_ready));
    value.set("result_seen", addon::Json_value(state.result_seen));
    value.set("response_has_tools", addon::Json_value(state.response_has_tools));
    auto servers = addon::Json_value::array();
    for (const auto& server : state.servers) {
        auto entry = addon::Json_value::object();
        entry.set("name", addon::Json_value(server.name));
        entry.set("tool_count", addon::Json_value(server.tool_count));
        entry.set("connected", addon::Json_value(server.connected));
        entry.set("tools_loaded", addon::Json_value(server.tools_loaded));
        servers.push_back(std::move(entry));
    }
    value.set("servers", std::move(servers));
    return addon::serialize_json(value);
}

std::optional<Session_state> decode_state(std::string_view bytes)
{
    const auto value = addon::parse_exchange_protocol_state(bytes);
    if (!value || !value->is_object() || value->as_object().size() != 16 ||
        integer_member(*value, "version") != 1)
    {
        return std::nullopt;
    }
    const auto* session_id = string_member(*value, "session_id");
    const auto* prompt_digest = string_member(*value, "prompt_digest");
    const auto* prompt_id = string_member(*value, "prompt_id");
    const auto* tail_base64 = string_member(*value, "tail_base64");
    const auto* answer = string_member(*value, "answer");
    const auto* response_id = string_member(*value, "response_id");
    const auto* last_message_id = string_member(*value, "last_message_id");
    const auto* chunk_text = string_member(*value, "chunk_text");
    const auto chunk_count = integer_member(*value, "chunk_count");
    const auto next_chunk = integer_member(*value, "next_chunk");
    const auto started = boolean_member(*value, "started");
    const auto answer_ready = boolean_member(*value, "answer_ready");
    const auto result_seen = boolean_member(*value, "result_seen");
    const auto response_has_tools = boolean_member(*value, "response_has_tools");
    const auto* servers = value->find("servers");
    if (!identity(session_id) || session_id->size() != 36 || !prompt_digest || prompt_digest->size() != 64 ||
        !prompt_id || prompt_id->size() > k_identity_limit || !tail_base64 ||
        !answer || answer->size() > k_stream_limit ||
        !response_id || response_id->size() > k_identity_limit ||
        !last_message_id || last_message_id->size() > k_identity_limit ||
        !chunk_text || chunk_text->size() > k_stream_limit || !chunk_count || !next_chunk ||
        *chunk_count < 0 || *chunk_count > static_cast<std::int64_t>(k_stream_limit) ||
        *next_chunk < 0 || (*chunk_count == 0 ? *next_chunk != 0 : *next_chunk >= *chunk_count) ||
        !started || !answer_ready || !result_seen || !response_has_tools ||
        !servers || !servers->is_array() || servers->as_array().size() > addon::limits::k_max_delegated_task_servers)
    {
        return std::nullopt;
    }
    const auto tail = addon::decode_base64(*tail_base64);
    if (!tail || tail->size() > k_stream_limit || tail->find('\n') != std::string::npos ||
        (!*started && (!prompt_id->empty() || *answer_ready || *result_seen)) ||
        (*answer_ready && prompt_id->empty()) || (*chunk_count == 0 && !chunk_text->empty()))
    {
        return std::nullopt;
    }
    Session_state state;
    state.session_id = *session_id;
    state.prompt_digest = *prompt_digest;
    state.prompt_id = *prompt_id;
    state.tail = *tail;
    state.answer = *answer;
    state.response_id = *response_id;
    state.last_message_id = *last_message_id;
    state.chunk_text = *chunk_text;
    state.chunk_count = *chunk_count;
    state.next_chunk = *next_chunk;
    state.started = *started;
    state.answer_ready = *answer_ready;
    state.result_seen = *result_seen;
    state.response_has_tools = *response_has_tools;
    for (const auto& entry : servers->as_array()) {
        const auto* name = string_member(entry, "name");
        const auto tool_count = integer_member(entry, "tool_count");
        const auto connected = boolean_member(entry, "connected");
        const auto tools_loaded = boolean_member(entry, "tools_loaded");
        if (!entry.is_object() || entry.as_object().size() != 4 || !identity(name) ||
            name->size() > k_native_tool_name_limit || !tool_count || *tool_count < 1 ||
            *tool_count > static_cast<std::int64_t>(addon::limits::k_max_delegated_server_actions) ||
            !connected || !tools_loaded ||
            std::any_of(state.servers.begin(), state.servers.end(),
                [&](const auto& server) { return server.name == *name; }))
        {
            return std::nullopt;
        }
        state.servers.push_back({*name, *tool_count, *connected, *tools_loaded});
    }
    return state;
}

std::optional<Prepared_session> prepare_session(
    const addon::Session_launch_context& launch, std::string_view prompt, addon::Invocation_context& context)
{
    if (!launch.launch_environment) {
        unavailable(context, "Copilot needs the launcher's account environment.");
        return std::nullopt;
    }
    const auto trimmed_prompt = trim_native_whitespace(prompt);
    if (trimmed_prompt.empty() || trimmed_prompt.starts_with('/') ||
        prompt.find('\0') != std::string_view::npos || !addon::is_valid_utf8(prompt))
    {
        unavailable(context, "The task requires a text prompt without a native slash-command prefix.");
        return std::nullopt;
    }
    auto environment = addon::environment_pairs(launch.launch_environment->effective_environment);
    const auto invocation = addon::literal_launcher_invocation(
        launch.launcher, launch.launch_environment->platform_id, launch.launcher_argv);
    if (launch.launcher.runner_key != addon::k_runner_direct || !invocation ||
        !launcher_invocation(launch.launcher, launch.launch_environment->platform_id) ||
        !fresh_session_arguments(invocation->argv, environment))
    {
        unavailable(context, "The selected invocation does not establish a fresh Direct Copilot task.");
        return std::nullopt;
    }
    if (!accept_account(context, launch.account_context, launch.launcher, *launch.launch_environment,
            invocation, launch.working_directory))
    {
        return std::nullopt;
    }
    Prepared_session prepared;
    prepared.environment = std::move(environment);
    const auto program = addon::resolve_program(
        invocation->argv.front(), prepared.environment, launch.working_directory);
    if (!program || !addon::native_image(*program)) {
        context.fail(addon::Result_status::UNAVAILABLE, addon::reason::k_launcher_program_unavailable,
            "This task requires a native Copilot executable.");
        return std::nullopt;
    }
    prepared.program = addon::path_to_utf8(*program);
    prepared.state.session_id = fresh_uuid();
    prepared.state.prompt_digest = digest(prompt);
    auto& process = prepared.process;
    process.executable = addon::Process_executable::launcher_program();
    process.argv.assign(invocation->argv.begin() + 1, invocation->argv.end());
    if (std::find(process.argv.begin(), process.argv.end(), "--no-auto-update") == process.argv.end()) {
        process.argv.emplace_back("--no-auto-update");
    }
    process.argv.push_back("--session-id=" + prepared.state.session_id);
    process.argv.emplace_back("--output-format=json");
    process.argv.emplace_back("--stream=off");
    process.stdin_behavior = addon::Stdin_behavior::PAYLOAD;
    // Copilot's UTF-8 stdin decoder consumes one initial byte-order mark.
    // Supply the encoding signature when the text itself starts with U+FEFF.
    if (prompt.starts_with("\uFEFF")) {
        process.stdin_payload = "\uFEFF";
    }
    process.stdin_payload.append(prompt);
    for (const auto& [name, value] : prepared.environment) {
        static_cast<void>(value);
        process.environment_names.push_back(name);
    }
    return prepared;
}

bool native_features(
    const Prepared_session& prepared, const addon::Session_launch_context& launch, bool delegated,
    addon::Invocation_context& context, const std::optional<addon::Delegated_model_selection>& selection = std::nullopt)
{
    const auto help = native_help(prepared.program, prepared.environment, launch.working_directory, context);
    if (context.cancel_requested()) {
        context.fail(addon::Result_status::CANCELLED, "cancelled");
        return false;
    }
    if (!help || !help->structured || (delegated && !help->delegated)) {
        unavailable(context, "The installed Copilot did not establish native task protocol support.");
        return false;
    }
    if (selection && selection->model && !help->model) {
        context.fail(addon::Result_status::UNAVAILABLE, addon::reason::k_model_selection_unavailable);
        return false;
    }
    if (selection && selection->reasoning_effort && !help->reasoning_effort) {
        context.fail(addon::Result_status::UNAVAILABLE, addon::reason::k_reasoning_effort_unavailable);
        return false;
    }
    return true;
}

void set_task_option(std::vector<std::string>& argv, std::string_view option, const std::string& value)
{
    // Copilot reads the last occurrence. An attached operand keeps values
    // starting with '-' literal without increasing the size of the saved argv.
    const std::string prefix = std::string(option) + '=';
    for (std::size_t index = argv.size(); index > 0; --index) {
        auto& argument = argv[index - 1];
        if (argument == option || argument.starts_with(prefix)) {
            if (argument == option) {
                argv.erase(argv.begin() + static_cast<std::ptrdiff_t>(index));
            }
            argument = prefix + value;
            return;
        }
    }
    argv.push_back(prefix + value);
}

bool native_identifier(std::string_view value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '-';
    });
}

bool attach_servers(
    const addon::Delegated_control& control, Prepared_session& prepared, addon::Delegated_prepare_result& result,
    addon::Invocation_context& context)
{
    if (!control.connection) {
        unavailable(context, "Delegated Copilot tasks require the host task-server connection.");
        return false;
    }
    auto servers = addon::Json_value::object();
    const auto prefix = "logonomic-" + addon::random_nonce_hex().substr(0, 16) + "-";
    for (std::size_t index = 0; index < control.servers.size(); ++index) {
        const auto& server = control.servers[index];
        const auto name = prefix + std::to_string(index);
        auto actions = addon::Json_value::array();
        if (server.actions.empty()) {
            unavailable(context, "A delegated task server must expose at least one requested action.");
            return false;
        }
        for (const auto& action : server.actions) {
            if (!native_identifier(action) || name.size() + 1 + action.size() > k_native_tool_name_limit) {
                unavailable(context, "The requested task action cannot retain its exact native MCP tool name.");
                return false;
            }
            actions.push_back(addon::Json_value(action));
            // Native deny rules still win. Only these host-owned action routes
            // receive a new allow rule; saved shell/path/URL policy is untouched.
            prepared.process.argv.push_back("--allow-tool=" + name + "(" + action + ")");
        }
        auto arguments = addon::Json_value::array();
        arguments.push_back(addon::Json_value("--descriptor"));
        arguments.push_back(addon::Json_value(control.connection->descriptor_path));
        arguments.push_back(addon::Json_value("--server"));
        arguments.push_back(addon::Json_value(server.server_id));
        auto entry = addon::Json_value::object();
        entry.set("type", addon::Json_value("local"));
        entry.set("command", addon::Json_value(control.connection->relay_program));
        entry.set("args", std::move(arguments));
        entry.set("tools", std::move(actions));
        entry.set("deferTools", addon::Json_value("never"));
        entry.set("disableToolCache", addon::Json_value(true));
        servers.set(name, std::move(entry));
        prepared.state.servers.push_back({name, static_cast<std::int64_t>(server.actions.size()), false, false});
        result.acknowledgment.server_ids.push_back(server.server_id);
    }
    auto config = addon::Json_value::object();
    config.set("mcpServers", std::move(servers));
    const std::string relative_path = "mcp/servers.json";
    const auto config_path =
        addon::path_from_utf8(control.private_artifact_root) / addon::path_from_utf8(relative_path);
    prepared.process.argv.push_back("--additional-mcp-config=@" + addon::path_to_utf8(config_path));
    result.private_artifacts.push_back({relative_path, addon::serialize_json(config)});
    result.acknowledgment.connection_version = addon::k_delegated_connection_version;
    return true;
}

bool all_servers_ready(const Session_state& state)
{
    return std::all_of(state.servers.begin(), state.servers.end(), [](const auto& server) {
        return server.connected && server.tools_loaded;
    });
}

Server_state* required_server(Session_state& state, std::string_view name)
{
    const auto found = std::find_if(state.servers.begin(), state.servers.end(), [&](const auto& server) {
        return server.name == name;
    });
    return found != state.servers.end() ? &*found : nullptr;
}

std::string server_status(Server_state& server, std::string_view status)
{
    if (status == "connected") {
        server.connected = true;
        return {};
    }
    if (status == "pending" && !server.connected) {
        server.tools_loaded = false;
        return {};
    }
    return "copilot_task_server_lost";
}

std::string inspect_readiness(Session_state& state, std::string_view type, const addon::Json_value& data)
{
    if (type == "session.mcp_servers_loaded") {
        const auto* servers = data.find("servers");
        if (!servers || !servers->is_array()) {
            return "copilot_mcp_event_invalid";
        }
        std::vector<std::string> seen;
        for (const auto& entry : servers->as_array()) {
            const auto* name = string_member(entry, "name");
            const auto* status = string_member(entry, "status");
            if (!identity(name) || !status) {
                return "copilot_mcp_event_invalid";
            }
            if (auto* server = required_server(state, *name)) {
                if (std::find(seen.begin(), seen.end(), *name) != seen.end()) {
                    return "copilot_mcp_event_invalid";
                }
                seen.push_back(*name);
                if (auto failure = server_status(*server, *status); !failure.empty()) {
                    return failure;
                }
            }
        }
        for (const auto& server : state.servers) {
            if (server.connected && std::find(seen.begin(), seen.end(), server.name) == seen.end()) {
                return "copilot_task_server_lost";
            }
        }
        return {};
    }
    const auto* name = string_member(data, "serverName");
    if (!identity(name)) {
        return "copilot_mcp_event_invalid";
    }
    auto* server = required_server(state, *name);
    if (!server) {
        return {};
    }
    if (type == "session.mcp_server_status_changed") {
        const auto* status = string_member(data, "status");
        return status ? server_status(*server, *status) : "copilot_mcp_event_invalid";
    }
    if (type == "session.mcp_server_tools_loaded") {
        const auto count = integer_member(data, "toolCount");
        if (!count || *count != server->tool_count || server->tools_loaded) {
            return "copilot_task_catalog_mismatch";
        }
        // The production host relay filters tools/list to control.actions.
        // Copilot counts that catalog, not its own tools configuration filter.
        server->tools_loaded = true;
        return {};
    }
    return "copilot_task_server_lost";
}

std::string inspect_answer(Session_state& state, const addon::Json_value& data)
{
    const auto* message_id = string_member(data, "messageId");
    const auto* originating_id = string_member(data, "originatingMessageId");
    const auto* content = string_member(data, "content");
    const auto* response_id = string_member(data, "apiCallId");
    if (!identity(message_id) || !originating_id || state.prompt_id.empty() || *originating_id != state.prompt_id ||
        !content || content->size() > k_stream_limit || *message_id == state.last_message_id ||
        (data.find("apiCallId") && !identity(response_id)) || !all_servers_ready(state))
    {
        return "copilot_response_unverified";
    }
    const auto* requests = data.find("toolRequests");
    if (requests && !requests->is_array()) {
        return "copilot_response_invalid";
    }
    const bool has_tools = requests && !requests->as_array().empty();
    const auto count = integer_member(data, "chunkCount");
    const auto index = integer_member(data, "chunkIndex");
    if (data.find("chunkCount") || data.find("chunkIndex")) {
        if (!count || !index || !identity(response_id) || *count < 1 ||
            *count > static_cast<std::int64_t>(k_stream_limit) || *index < 0 || *index >= *count)
        {
            return "copilot_response_invalid";
        }
        if (state.chunk_count == 0) {
            if (*index != 0 || *response_id == state.response_id) {
                return "copilot_response_invalid";
            }
            state.response_id = *response_id;
            state.chunk_count = *count;
            state.next_chunk = 0;
            state.chunk_text.clear();
            state.response_has_tools = false;
        }
        if (*response_id != state.response_id || *count != state.chunk_count || *index != state.next_chunk ||
            content->size() > k_stream_limit - state.chunk_text.size())
        {
            return "copilot_response_invalid";
        }
        state.chunk_text += *content;
        state.response_has_tools = state.response_has_tools || has_tools;
        ++state.next_chunk;
        if (state.next_chunk == state.chunk_count) {
            state.answer = std::move(state.chunk_text);
            state.chunk_text.clear();
            state.answer_ready = !state.response_has_tools;
            state.chunk_count = 0;
            state.next_chunk = 0;
            state.response_has_tools = false;
        }
    }
    else {
        if (state.chunk_count != 0 || (response_id && *response_id == state.response_id)) {
            return "copilot_response_invalid";
        }
        state.response_id = response_id ? *response_id : "";
        state.answer = *content;
        state.answer_ready = !has_tools;
    }
    state.last_message_id = *message_id;
    return {};
}

std::string inspect_line(
    Session_state& state, std::string_view line, bool observe_model, addon::Structured_inspect_result& result)
{
    addon::Json_parse_limits limits;
    limits.max_bytes = k_stream_limit;
    limits.max_string_bytes = k_stream_limit;
    const auto event = addon::parse_json(line, limits, nullptr);
    if (!event || !event->is_object()) {
        return "copilot_output_invalid";
    }
    const auto* type = string_member(*event, "type");
    if (!identity(type)) {
        return "copilot_output_invalid";
    }
    if (const auto* agent = event->find("agentId")) {
        if (!identity(agent->is_string() ? &agent->as_string() : nullptr)) {
            return "copilot_agent_invalid";
        }
        return {};
    }
    if (state.result_seen) {
        return "copilot_output_after_result";
    }
    if (*type == "result") {
        const auto* session_id = string_member(*event, "sessionId");
        const auto exit_code = integer_member(*event, "exitCode");
        if (!session_id || *session_id != state.session_id || !exit_code || *exit_code != 0 ||
            !state.started || state.prompt_id.empty() || !state.answer_ready || state.chunk_count != 0 ||
            !all_servers_ready(state))
        {
            return "copilot_result_invalid";
        }
        state.result_seen = true;
        return {};
    }
    const auto* data = event->find("data");
    if (!data || !data->is_object()) {
        return "copilot_output_invalid";
    }
    if (const auto* parent_tool = data->find("parentToolCallId")) {
        if (!identity(parent_tool->is_string() ? &parent_tool->as_string() : nullptr)) {
            return "copilot_agent_invalid";
        }
        return {};
    }
    if (*type == "session.error") {
        return "copilot_session_error";
    }
    if (*type == "session.resume") {
        return "copilot_session_not_fresh";
    }
    const bool authoritative = *type == "session.start" || *type == "user.message" || *type == "assistant.message";
    if (authoritative && event->find("checkpoint") && boolean_member(*event, "checkpoint") != false) {
        return "copilot_session_not_fresh";
    }
    if (*type == "session.start") {
        const auto* session_id = string_member(*data, "sessionId");
        if (state.started || !session_id || *session_id != state.session_id ||
            (data->find("alreadyInUse") && boolean_member(*data, "alreadyInUse") != false))
        {
            return "copilot_session_mismatch";
        }
        state.started = true;
    }
    else
    if (*type == "user.message") {
        const auto* content = string_member(*data, "content");
        const auto* message_id = string_member(*data, "messageId");
        if (!state.started || !state.prompt_id.empty() || !identity(message_id) ||
            !content || digest(*content) != state.prompt_digest)
        {
            return "copilot_prompt_mismatch";
        }
        state.prompt_id = *message_id;
    }
    else
    if (*type == "assistant.message") {
        if (!state.started) {
            return "copilot_session_mismatch";
        }
        const auto failure = inspect_answer(state, *data);
        if (failure.empty() && observe_model && data->find("model")) {
            const auto* model = string_member(*data, "model");
            if (model && addon::is_valid_model_label(*model)) {
                result.model_observation = addon::Structured_model_observation{"", *model, "assistant.message"};
                result.model_failure.reset();
            }
            else
            if (!result.model_observation) {
                result.model_failure = addon::Contract_failure{"invalid_model_observation", "Invalid Copilot model label."};
            }
        }
        return failure;
    }
    else
    if (*type == "session.mcp_servers_loaded" || *type == "session.mcp_server_status_changed" ||
        *type == "session.mcp_server_tools_loaded" || *type == "session.mcp_server_removed" ||
        *type == "session.mcp_server_needs_reconnect")
    {
        if (!state.started) {
            return "copilot_session_mismatch";
        }
        return inspect_readiness(state, *type, *data);
    }
    return {};
}

std::string inspect_output(
    Session_state& state, const addon::Structured_inspection& inspection,
    bool observe_model, addon::Structured_inspect_result& result)
{
    std::string_view input = inspection.stdout_bytes;
    while (!input.empty()) {
        const auto newline = input.find('\n');
        const auto segment = input.substr(0, newline);
        if (segment.size() > k_stream_limit - state.tail.size()) {
            return "copilot_output_too_large";
        }
        state.tail.append(segment);
        if (newline == std::string_view::npos) {
            break;
        }
        if (auto reason = inspect_line(state, state.tail, observe_model, result); !reason.empty()) {
            return reason;
        }
        state.tail.clear();
        input.remove_prefix(newline + 1);
    }
    if (inspection.final && !state.tail.empty()) {
        return inspect_line(state, state.tail, observe_model, result);
    }
    return {};
}

void prepare_one_shot(addon::Invocation_context& context, bool authoring)
{
    addon::Contract_failure failure;
    const auto request = authoring ? addon::decode_authoring_prepare_request(context.request().payload, &failure) :
        addon::decode_structured_prepare_request(context.request().payload, &failure);
    if (!request) {
        if (authoring && failure.reason_code == addon::reason::k_purpose_unavailable) {
            context.fail(addon::Result_status::UNAVAILABLE, failure.reason_code, failure.detail);
        }
        else {
            reject(context, failure);
        }
        return;
    }
    if (!authoring && !implementation(request->purpose, context)) {
        return;
    }
    auto prepared = prepare_session(request->context, request->prompt, context);
    if (!prepared) {
        return;
    }
    addon::Structured_prepare_result result;
    result.process = prepared->process;
    if (authoring) {
        if (!configure_authoring(result, *request, request->context.launcher_argv, prepared->state.session_id)) {
            unavailable(context, "This launcher cannot supply a text-only Copilot session.");
            return;
        }
        if (!supports_authoring(prepared->program, prepared->environment, request->context.working_directory, context)) {
            if (context.cancel_requested()) {
                context.fail(addon::Result_status::CANCELLED, "cancelled");
            }
            else {
                unavailable(context, "Workflow authoring requires the standalone Copilot CLI 1.0.112 or later.");
            }
            return;
        }
    }
    else {
        result.inspection = {true, encode_state(prepared->state), false};
    }
    result.profile_storage = request->context.profile_storage;
    result.account_context = request->context.account_context;
    const auto payload = addon::encode_structured_prepare_result(result);
    if (!addon::validate_structured_process_proposal(result.process, &failure) ||
        !addon::validate_session_exchange_payload(payload, &failure))
    {
        context.fail(addon::Result_status::UNAVAILABLE, failure.reason_code, failure.detail);
        return;
    }
    if (authoring || native_features(*prepared, request->context, false, context)) {
        context.succeed(payload);
    }
}

} // namespace

void prepare_structured(addon::Invocation_context& context)
{
    prepare_one_shot(context, false);
}

void prepare_authoring(addon::Invocation_context& context)
{
    prepare_one_shot(context, true);
}

void prepare_delegated(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_delegated_prepare_request(context.request().payload, &failure);
    if (!request) {
        reject(context, failure);
        return;
    }
    if (!implementation(request->purpose, context)) {
        return;
    }
    static constexpr addon::Delegated_model_selection_declaration selection_support{true, true};
    if (request->model_selection &&
        !addon::validate_delegated_model_selection(*request->model_selection, &selection_support, &failure))
    {
        context.fail(addon::Result_status::UNAVAILABLE, failure.reason_code, failure.detail);
        return;
    }
    auto prepared = prepare_session(request->context, request->prompt, context);
    if (!prepared) {
        return;
    }
    addon::Delegated_prepare_result result;
    if (!attach_servers(request->control, *prepared, result, context)) {
        return;
    }
    if (request->model_selection) {
        const auto& selection = *request->model_selection;
        if (selection.model) {
            set_task_option(prepared->process.argv, "--model", *selection.model);
        }
        if (selection.reasoning_effort) {
            set_task_option(prepared->process.argv, "--reasoning-effort", *selection.reasoning_effort);
        }
        result.model_selection = selection;
    }
    result.process = prepared->process;
    result.inspection = {true, encode_state(prepared->state), false};
    result.profile_storage = request->context.profile_storage;
    result.account_context = request->context.account_context;
    const auto payload = addon::encode_delegated_prepare_result(result);
    if (!addon::validate_structured_process_proposal(result.process, &failure) ||
        !addon::validate_delegated_acknowledgment(request->control, result.acknowledgment, &failure) ||
        !addon::decode_delegated_prepare_result(payload, &failure) ||
        !addon::validate_session_exchange_payload(payload, &failure))
    {
        context.fail(addon::Result_status::UNAVAILABLE, failure.reason_code, failure.detail);
        return;
    }
    if (native_features(*prepared, request->context, true, context, request->model_selection)) {
        context.succeed(payload);
    }
}

void inspect_session(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_structured_inspect_request(context.request().payload, &failure);
    if (!request) {
        reject(context, failure);
        return;
    }
    if (!implementation(request->purpose, context)) {
        return;
    }
    addon::Structured_inspect_result result;
    result.model_failure = request->model_failure;
    const bool observe_model = request->model_state_version == addon::k_model_state_version;
    const auto fail = [&](std::string reason) {
        addon::Structured_inspect_result failed;
        failed.complete = true;
        failed.failure_reason = std::move(reason);
        failed.model_observation = std::move(result.model_observation);
        failed.model_failure = std::move(result.model_failure);
        context.succeed(addon::encode_structured_inspect_result(failed));
    };
    auto state = decode_state(request->inspection.protocol_state);
    if (!state) {
        fail("copilot_state_invalid");
        return;
    }
    const auto& inspection = request->inspection;
    const bool process_failed = inspection.final && (inspection.timed_out || inspection.exit_code != 0);
    // Final stdout can contain the operation's last confirmed model even
    // when a later native error or process exit makes the task fail.
    std::string output_failure;
    if (!process_failed || observe_model) {
        output_failure = inspect_output(*state, inspection, observe_model, result);
    }
    if (process_failed) {
        fail(inspection.timed_out ? "copilot_timeout" : "copilot_process_failed");
        return;
    }
    if (!output_failure.empty()) {
        fail(std::move(output_failure));
        return;
    }
    result.complete = inspection.final;
    if (inspection.final) {
        if (!state->started || state->prompt_id.empty() || !state->answer_ready || !state->result_seen ||
            state->chunk_count != 0 || !all_servers_ready(*state))
        {
            fail("copilot_result_incomplete");
            return;
        }
        result.result_bytes = std::move(state->answer);
    }
    else {
        result.protocol_state = encode_state(*state);
        if (!addon::exchange_protocol_state_valid(result.protocol_state)) {
            fail("copilot_state_too_large");
            return;
        }
    }
    const auto payload = addon::encode_structured_inspect_result(result);
    if (!addon::validate_session_exchange_payload(payload, &failure)) {
        fail("copilot_result_too_large");
        return;
    }
    context.succeed(payload);
}

} // namespace copilot
