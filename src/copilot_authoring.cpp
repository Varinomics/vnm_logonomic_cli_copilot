#include "copilot_authoring.h"

#include "logonomic_cli_addon/capability_ids.h"
#include "logonomic_cli_addon/package_source.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <utility>

namespace copilot {
namespace {

namespace addon = logonomic::cli_addon;
using Json = addon::Json_value;

constexpr auto k_stream_limit = addon::limits::k_max_structured_stream_bytes;
enum Step { CONNECT = 1, CREATE, QUERY, SHUTDOWN, FINISHED };

Json object(std::initializer_list<addon::Json_member> members)
{
    return Json(Json::Object(members));
}

std::string frame(int id, const char* method, Json params)
{
    const auto bytes = addon::serialize_json(object({
        {"jsonrpc", Json("2.0")}, {"id", Json(id)}, {"method", Json(method)}, {"params", std::move(params)}
    }));
    return "Content-Length: " + std::to_string(bytes.size()) + "\r\n\r\n" + bytes;
}

const std::string* text(const Json& value, const char* key)
{
    const auto* member = value.find(key);
    return member && member->is_string() ? &member->as_string() : nullptr;
}

std::optional<Json> parse(std::string_view bytes, std::size_t limit)
{
    addon::Json_parse_limits limits;
    limits.max_bytes = limit;
    limits.max_string_bytes = limit;
    return addon::parse_json(bytes, limits, nullptr);
}

bool supported_version(std::string_view version)
{
    std::array<unsigned, 3> numbers{};
    for (std::size_t i = 0; i < numbers.size(); ++i) {
        if (version.empty()) {
            return false;
        }
        const auto parsed = std::from_chars(version.data(), version.data() + version.size(), numbers[i]);
        if (parsed.ec != std::errc()) {
            return false;
        }
        version.remove_prefix(static_cast<std::size_t>(parsed.ptr - version.data()));
        if (i + 1 < numbers.size()) {
            if (!version.starts_with('.')) {
                return false;
            }
            version.remove_prefix(1);
        }
    }
    return version.empty() && numbers >= std::array<unsigned, 3>{1, 0, 112};
}

Json session_options(Json options, int64_t protocol)
{
    // Protocol 3 uses flat session settings. Protocol 4 groups the same
    // settings by domain. Both define ephemeralQuery as a no-tools query.
    if (protocol == 3) {
        if (const auto* directory = text(options, "configDirectory")) {
            options.set("configDir", Json(*directory));
            options.erase("configDirectory");
        }
        options.set("availableTools", Json::array());
        options.set("includedBuiltinAgents", Json::array());
        options.set("customAgentsLocalOnly", Json(true));
        options.set("requestExtensions", Json(false));
        options.set("enableSkills", Json(false));
        options.set("enableFileHooks", Json(false));
        options.set("enableHostGitOperations", Json(false));
        options.set("enableOnDemandInstructionDiscovery", Json(false));
        options.set("enableSessionStore", Json(false));
        options.set("skipEmbeddingRetrieval", Json(true));
        options.set("memory", object({{"enabled", Json(false)}}));
        return options;
    }
    if (const auto* effort = text(options, "reasoningEffort")) {
        options.set("inference", object({{"reasoningEffort", Json(*effort)}}));
        options.erase("reasoningEffort");
    }
    for (auto& [name, value] : Json::Object{
        {"tools", object({{"availableTools", Json::array()}})},
        {"hooks", object({{"enableRepositoryHooks", Json(false)}})},
        {"agents", object({{"includedBuiltinAgents", Json::array()}, {"localDiscoveryOnly", Json(true)}})},
        {"extensions", object({{"enabled", Json(false)}})}, {"skills", object({{"enabled", Json(false)}})},
        {"git", object({{"enableHostGitOperations", Json(false)}})},
        {"instructions", object({{"onDemandDiscovery", Json(false)}})},
        {"state", object({{"skipEmbeddingRetrieval", Json(true)}, {"enableCrossSessionStore", Json(false)},
            {"memory", object({{"enabled", Json(false)}})}})}
    }) {
        options.set(std::move(name), std::move(value));
    }
    return options;
}

bool apply_launcher_options(const std::vector<std::string>& arguments, Json& options, std::vector<std::string>& argv)
{
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const std::string_view word = arguments[i];
        if (word == "--no-auto-update" || word == "--no-custom-instructions" || word == "--disable-builtin-mcps") {
            continue;
        }
        const auto equals = word.find('=');
        const auto name = word.substr(0, equals);
        if (name != "--model" && name != "--reasoning-effort" && name != "--config-dir") {
            return false;
        }
        if (equals == word.npos && ++i == arguments.size()) {
            return false;
        }
        const auto value = equals == word.npos ? std::string_view(arguments[i]) : word.substr(equals + 1);
        if (value.empty() || value.front() == '-') {
            return false;
        }
        if (name == "--config-dir") {
            options.set("configDirectory", Json(value));
            argv.push_back("--config-dir=" + std::string(value));
        }
        else if (name == "--model") {
            options.set("model", Json(value));
        }
        else {
            options.set("reasoningEffort", Json(value));
        }
    }
    return true;
}

bool consume_message(const Json& message, Json& state, addon::Structured_inspect_result& reply)
{
    const auto* version = text(message, "jsonrpc");
    const auto* id      = message.find("id");
    const auto* step    = state.find("step");
    if (!version || *version != "2.0" || !step || !step->is_integer()) {
        return false;
    }
    if (message.find("method")) {
        // Notifications include transcript/progress data. No server-initiated
        // request (including a permission or tool callback) is actionable here.
        return !id && text(message, "method") && !message.find("result") && !message.find("error");
    }
    const auto* result = message.find("result");
    if (!id || *id != *step || !result || message.find("error") || reply.stdin_bytes) {
        return false;
    }
    switch (step->as_integer()) {
    case CONNECT: {
        const auto* ok = result->find("ok");
        const auto* protocol = result->find("protocolVersion");
        const auto* options = state.find("options");
        const auto* package = text(*result, "version");
        if (!ok || !ok->is_boolean() || !ok->as_boolean() || !protocol || !protocol->is_integer() ||
            (protocol->as_integer() != 3 && protocol->as_integer() != 4) ||
            !package || !supported_version(*package) || !options || !options->is_object()) {
            return false;
        }
        reply.stdin_bytes = frame(CREATE, "session.create", session_options(*options, protocol->as_integer()));
        state.erase("options");
        state.set("step", Json(CREATE));
        return true;
    }
    case CREATE: {
        const auto* expected = text(state, "sessionId");
        const auto* actual   = text(*result, "sessionId");
        const auto* question = text(state, "question");
        if (!expected || !actual || *actual != *expected || !question) {
            return false;
        }
        // This method is a transient model query whose native contract supplies
        // no tools. Sending a normal session.send turn here would grant a
        // different contract. An older server must reject the unknown method.
        reply.stdin_bytes = frame(QUERY, "session.ui.ephemeralQuery",
            object({{"sessionId", Json(*actual)}, {"question", Json(*question)}}));
        state.erase("question");
        state.set("step", Json(QUERY));
        return true;
    }
    case QUERY: {
        const auto* answer = text(*result, "answer");
        if (!answer || answer->empty() || answer->size() > k_stream_limit || answer->find('\0') != answer->npos) {
            return false;
        }
        state.set("answer", Json(*answer));
        state.set("step", Json(SHUTDOWN));
        reply.stdin_bytes = frame(SHUTDOWN, "runtime.shutdown", Json::object());
        return true;
    }
    case SHUTDOWN:
        if (!result->is_null()) {
            return false;
        }
        state.set("step", Json(FINISHED));
        return true;
    default:
        return false;
    }
}

bool consume_frames(std::string_view bytes, Json& state, addon::Structured_inspect_result& reply)
{
    // vscode-jsonrpc writes Content-Length in UTF-8 bytes. An incoming
    // packet can split the header, a code point, or contain several frames.
    constexpr std::string_view prefix = "Content-Length: ";
    while (!bytes.empty()) {
        const auto header = bytes.find("\r\n\r\n");
        if (header == bytes.npos) {
            if (bytes.size() > 256) {
                return false;
            }
            break;
        }
        if (header > 256 || !bytes.starts_with(prefix)) {
            return false;
        }
        const auto* first = bytes.data() + prefix.size();
        const auto* last = bytes.data() + header;
        std::size_t length = 0;
        if (first >= last) {
            return false;
        }
        const auto parsed = std::from_chars(first, last, length);
        if (parsed.ec != std::errc() || parsed.ptr != last || length == 0 || length > k_stream_limit) {
            return false;
        }
        if (bytes.size() - header - 4 < length) {
            break;
        }
        const auto message = parse(bytes.substr(header + 4, length), k_stream_limit);
        if (!message || !consume_message(*message, state, reply)) {
            return false;
        }
        bytes.remove_prefix(header + 4 + length);
    }
    state.set("tail", Json(addon::encode_base64(bytes)));
    return true;
}

} // namespace

bool supports_authoring(const std::string& program, const addon::Environment& environment,
    std::string_view working_directory, addon::Invocation_context& context)
{
    // VS Code installs a PATH shim that can contact its editor process.
    // Only the standalone CLI and npm's standalone platform binary qualify.
    const auto directory = addon::path_from_utf8(program).parent_path().filename();
    if (addon::lower(addon::path_to_utf8(directory)) == "copilot-shim") {
        return false;
    }
    const auto timeout = std::min<int64_t>(45000, context.request().deadline_ms);
    const auto result = addon::probe({program, "--no-auto-update", "--version"}, environment, working_directory,
        static_cast<int>(timeout), context);
    if (!result) {
        return false;
    }
    constexpr std::string_view banner = "GitHub Copilot CLI ";
    auto version = std::string_view(result->output).substr(0, result->output.find_first_of("\r\n"));
    if (version.starts_with(banner)) {
        version.remove_prefix(banner.size());
        if (version.ends_with('.')) {
            version.remove_suffix(1);
        }
    }
    return supported_version(version);
}

bool configure_authoring(addon::Structured_prepare_result& result, const addon::Structured_prepare_request& request,
    const std::vector<std::string>& launcher_arguments, std::string session_id)
{
    auto options = object({{"sessionId", Json(session_id)}, {"workingDirectory", Json(request.context.working_directory)},
        {"enableConfigDiscovery", Json(false)}});
    auto& process = result.process;
    process.argv = {"--headless", "--no-auto-update", "--stdio"};
    if (!apply_launcher_options(launcher_arguments, options, process.argv)) {
        return false;
    }
    process.stdin_payload = frame(CONNECT, "connect", object({{"supportedTaskKinds", Json::array()}}));
    auto state = object({{"step", Json(CONNECT)}, {"sessionId", Json(session_id)}, {"options", std::move(options)},
        {"question", Json(request.prompt)}, {"tail", Json("")}});
    result.inspection = {true, addon::serialize_json(state), true};
    return true;
}

void inspect_authoring(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_authoring_inspect_request(context.request().payload, &failure);
    if (!request) {
        context.fail(failure.reason_code == addon::reason::k_purpose_unavailable ?
            addon::Result_status::UNAVAILABLE : addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    const auto& inspection = request->inspection;
    auto state = parse(inspection.protocol_state, addon::limits::k_max_exchange_protocol_state_utf8_bytes);
    const auto* carry = state ? text(*state, "tail") : nullptr;
    auto tail = carry ? addon::decode_base64(*carry) : std::nullopt;
    addon::Structured_inspect_result reply;
    if (state && tail && tail->size() <= k_stream_limit + 256) {
        tail->append(inspection.stdout_bytes);
        if (consume_frames(*tail, *state, reply)) {
            const auto* step = state->find("step");
            if (step && *step == Json(FINISHED) && *state->find("tail") == Json("") &&
                !inspection.timed_out && (!inspection.final || inspection.exit_code == 0))
            {
                const auto* answer = text(*state, "answer");
                if (answer && !answer->empty()) {
                    addon::Structured_inspect_result completed;
                    completed.complete = true;
                    completed.result_bytes = *answer;
                    context.succeed(addon::encode_structured_inspect_result(completed));
                    return;
                }
            }
            else if (!inspection.final) {
                reply.protocol_state = addon::serialize_json(*state);
                if (addon::exchange_protocol_state_valid(reply.protocol_state)) {
                    context.succeed(addon::encode_structured_inspect_result(reply));
                    return;
                }
            }
        }
    }
    addon::Structured_inspect_result failed;
    failed.complete = true;
    failed.failure_reason = inspection.timed_out ? "copilot_timeout" : "copilot_authoring_unavailable";
    context.succeed(addon::encode_structured_inspect_result(failed));
}

} // namespace copilot
