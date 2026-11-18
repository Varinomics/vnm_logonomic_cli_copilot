#include "copilot_usage.h"

#include "copilot_accounts.h"
#include "copilot_usage_journal.h"

#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>
#include <logonomic_cli_addon/usage_scan.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <map>
#include <stdexcept>

namespace copilot {
namespace {
namespace addon = logonomic::cli_addon;
namespace json = addon::json;
namespace fs = std::filesystem;
using Json = addon::Json_value;

constexpr std::uint64_t k_max_exact = 9007199254740991ULL;
constexpr std::size_t k_rows_per_page = 64;
constexpr std::size_t k_page_bytes = 1024 * 1024;
constexpr std::string_view k_format = "copilot-native-receipts@1";
constexpr std::string_view k_namespace = "github.copilot.usage-record";

class Scan_error final : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

void require(bool condition, std::string_view code)
{
    if (!condition) {
        throw Scan_error(std::string(code));
    }
}

std::string digest(std::string_view bytes)
{
    return addon::to_hex(addon::Sha256::digest(bytes));
}

std::string request_digest(addon::Usage_scan_request request)
{
    request.snapshot_id.reset();
    request.cursor.reset();
    return digest(addon::serialize_json_canonical(addon::encode_usage_scan_request(request)));
}

bool uuid(std::string_view value)
{
    if (value.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto c = value[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (c != '-') return false;
        }
        else
        if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f') && !(c >= 'A' && c <= 'F')) {
            return false;
        }
    }
    return true;
}

std::optional<std::uint64_t> count(const Json& value, const char* name)
{
    const auto* part = value.find(name);
    if (!part) return std::nullopt;
    require(part->is_integer() && part->as_integer() >= 0 &&
        static_cast<std::uint64_t>(part->as_integer()) <= k_max_exact, "usage_counter_invalid");
    return static_cast<std::uint64_t>(part->as_integer());
}

addon::Usage_amount dimension(std::optional<std::uint64_t> value)
{
    return {true, value, addon::Usage_field_coverage{value ? 1ULL : 0ULL, 1, false,
        value ? addon::Usage_support::SUPPORTED : addon::Usage_support::UNKNOWN}};
}

struct Receipt
{
    std::string usage_id_sha256;
    std::string timestamp;
    addon::Usage_record record;
};
using Ledger = std::map<std::string, Receipt, std::less<>>;

Json encode(const Receipt& receipt)
{
    return json::object({{"usage_id_sha256", Json(receipt.usage_id_sha256)}, {"timestamp", Json(receipt.timestamp)},
        {"record", addon::encode_usage_record(receipt.record)}});
}

void add_receipt(Ledger& ledger, Receipt receipt)
{
    const auto [entry, inserted] = ledger.try_emplace(receipt.record.record_id, std::move(receipt));
    require(inserted || encode(entry->second) == encode(receipt), "usage_receipt_conflict");
}

void native_receipt(const Json& event, Ledger& ledger)
{
    const auto& usage = json::member(json::member(event, "data"), "usage");
    const auto& identity = json::member(usage, "accounting");
    const auto session = json::text(identity, "sourceSessionId");
    const auto sequence = count(identity, "sequence");
    const auto usage_id = json::text(identity, "usageId");
    const auto input = count(usage, "inputTokens");
    const auto output = count(usage, "outputTokens");
    if (!input || !output || !identity.is_object()) {
        // Older sessions and backends without measured counters have limited history.
        return;
    }
    require(uuid(session) && sequence && *sequence > 0 && !usage_id.empty(),
        "usage_identity_invalid");
    require(*input <= k_max_exact - *output, "usage_counter_invalid");
    const auto stamp = json::text(event, "timestamp");
    require(stamp.size() == 24 && stamp[10] == 'T' && stamp[19] == '.' && stamp.back() == 'Z' &&
        addon::parse_native_usage_timestamp(stamp), "usage_timestamp_invalid");
    Receipt receipt{digest(usage_id), stamp, {}};
    auto& record = receipt.record;
    record.record_id = "copilot:" + session + ":" + std::to_string(*sequence);
    record.session_key = session;
    record.completed_date = stamp.substr(0, 10);
    record.total_tokens = *input + *output;
    record.input = dimension(input);
    record.output = dimension(output);
    const auto& cache_reported = json::member(usage, "cacheDetailsReported");
    const bool cache_known = cache_reported.is_boolean() && cache_reported.as_boolean();
    record.cache_read = dimension(cache_known ? count(usage, "cacheReadTokens") : std::nullopt);
    record.cache_creation = dimension(cache_known ? count(usage, "cacheWriteTokens") : std::nullopt);
    const auto* model = usage.find("responseModel");
    if (model && json::bounded_string(*model, 256)) {
        record.model = model->as_string();
    }
    record.semantics = addon::Usage_counter_semantics::DELTA_EVENT;
    record.source_counter_semantics = "per_event_sum";
    record.aggregate_evidence = "complete";
    // A user can create a new session with the same --session-id in another
    // profile. Only the original immutable event UUID can attest a shared call.
    const auto event_id = json::text(event, "id");
    if (uuid(event_id)) {
        const auto members = Json(Json::Array{Json(event_id), Json(record.record_id), Json(receipt.usage_id_sha256)});
        record.cross_source_event = addon::Usage_cross_source_event{std::string(k_namespace),
            digest(addon::serialize_json_canonical(members))};
    }
    require(addon::decode_usage_record(addon::encode_usage_record(record), nullptr).has_value(), "usage_record_invalid");
    add_receipt(ledger, std::move(receipt));
}

class Progress
{
public:
    explicit Progress(addon::Invocation_context& context) : m_context(context) {}

    bool cancelled()
    {
        if (m_context.cancel_requested()) return true;
        const auto now = std::chrono::steady_clock::now();
        if (now - m_last >= std::chrono::seconds(1)) {
            m_context.report_progress(m_examined, std::nullopt, "Reading Copilot usage");
            m_last = now;
        }
        return false;
    }

    void advance()
    {
        ++m_examined;
        require(!cancelled(), "cancelled");
    }

private:
    addon::Invocation_context& m_context;
    std::chrono::steady_clock::time_point m_last{};
    std::uint64_t m_examined = 0;
};

void scan_journals(const fs::path& profile, Ledger& ledger, Progress& progress)
{
    require(fs::is_directory(profile), "usage_profile_unavailable");
    const auto root = profile / "session-state";
    std::error_code error;
    const auto status = fs::symlink_status(root, error);
    if (error == std::errc::no_such_file_or_directory || status.type() == fs::file_type::not_found) return;
    require(!error && fs::is_directory(status), "usage_source_unavailable");
    for (const auto& entry : fs::directory_iterator(root)) {
        progress.advance();
        const auto session = addon::path_to_utf8(entry.path().filename());
        if (!uuid(session)) continue;
        require(fs::is_directory(entry.symlink_status()), "usage_source_unavailable");
        bool started = false;
        read_usage_journal(profile, session, [&](std::string_view line) {
            progress.advance();
            const auto event = addon::parse_json(line, {line.size(), 128, 65536, line.size()}, nullptr);
            require(event && event->is_object(), "usage_journal_invalid");
            const auto type = json::text(*event, "type");
            if (!started) {
                require(type == "session.start" && json::text(json::member(*event, "data"), "sessionId") == session,
                    "usage_journal_identity_invalid");
                started = true;
            }
            else
            if (type == "session.usage_record") {
                native_receipt(*event, ledger);
            }
        }, [&] { require(!progress.cancelled(), "cancelled"); });
    }
}

std::string page_name(std::uint64_t index)
{
    return "receipts-" + std::to_string(index) + ".json";
}

void write_json(const fs::path& path, const Json& value)
{
    const auto bytes = addon::serialize_json(value);
    require(bytes.size() <= k_page_bytes, "usage_page_too_large");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    require(file.good(), "usage_cache_write_failed");
}

Json read_json(const fs::path& root, std::string_view name)
{
    const auto bytes = addon::Directory_package_source(root).read(name, k_page_bytes, nullptr);
    require(bytes.has_value(), "usage_cache_invalid");
    const auto parsed = addon::parse_json(*bytes, {k_page_bytes, 32, 1024, k_page_bytes}, nullptr);
    require(parsed.has_value(), "usage_cache_invalid");
    return *parsed;
}

struct Snapshot
{
    addon::Usage_cache_reference reference;
    std::uint64_t observed_at = 0;
    std::uint64_t page_count = 0;
    std::optional<addon::Usage_cache_manifest> manifest;
};

Snapshot open_snapshot(const addon::Usage_cache_reference& reference, const addon::Usage_scan_request& request,
    const fs::path& profile, bool continuation, Progress& progress)
{
    addon::Contract_failure failure;
    const auto& store = request.context.profile_storage->root;
    const auto cancelled = [&] { return progress.cancelled(); };
    auto manifest = continuation
        ? addon::validate_usage_cache_manifest(reference, store, request.context.account_context, cancelled, &failure)
        : addon::validate_usage_cache_candidate(reference, store, request.context.account_context, cancelled, &failure);
    require(manifest && manifest->format == k_format, "usage_cache_invalid");
    if (continuation) {
        require(addon::validate_usage_cache_file(reference, *manifest, "snapshot.json", cancelled, &failure),
            "usage_cache_invalid");
    }
    const auto root = addon::usage_cache_io_path(addon::path_from_utf8(reference.root));
    const auto meta = read_json(root, "snapshot.json");
    require(json::text(meta, "profile") == addon::path_to_utf8(profile), "usage_profile_changed");
    if (continuation) {
        require(json::text(meta, "request") == request_digest(request), "usage_snapshot_context_changed");
    }
    const auto observed = count(meta, "observed");
    const auto pages = count(meta, "pages");
    require(observed && pages && *pages > 0 && *pages == manifest->files.size() - 1, "usage_cache_invalid");
    return {reference, *observed, *pages, continuation ? std::move(manifest) : std::nullopt};
}

std::vector<Receipt> load_page(const Snapshot& snapshot, std::uint64_t index)
{
    const auto root = addon::usage_cache_io_path(addon::path_from_utf8(snapshot.reference.root));
    const auto rows = read_json(root, page_name(index));
    require(rows.is_array() && rows.as_array().size() <= k_rows_per_page, "usage_cache_invalid");
    std::vector<Receipt> receipts;
    for (const auto& row : rows.as_array()) {
        auto record = addon::decode_usage_record(json::member(row, "record"), nullptr);
        require(record.has_value(), "usage_cache_invalid");
        receipts.push_back({json::text(row, "usage_id_sha256"), json::text(row, "timestamp"), std::move(*record)});
    }
    return receipts;
}

Snapshot create_snapshot(const addon::Usage_scan_request& request, const fs::path& profile, Progress& progress)
{
    const auto& selection = *request.cache_selection;
    addon::Contract_failure failure;
    const bool valid = addon::validate_usage_cache_allocation(selection.candidate_root,
        request.context.profile_storage->root, &failure);
    require(valid, failure.reason_code);
    Ledger ledger;
    if (selection.accepted) {
        const auto accepted = open_snapshot(*selection.accepted, request, profile, false, progress);
        for (std::uint64_t index = 0; index < accepted.page_count; ++index) {
            progress.advance();
            for (auto& old : load_page(accepted, index)) add_receipt(ledger, std::move(old));
        }
    }
    scan_journals(profile_io_path(profile), ledger, progress);
    const auto root = addon::usage_cache_io_path(addon::path_from_utf8(selection.candidate_root));
    std::uint64_t pages = 0;
    auto next = ledger.begin();
    do {
        auto rows = Json::array();
        while (next != ledger.end() && rows.as_array().size() < k_rows_per_page) {
            progress.advance();
            rows.push_back(encode(next->second));
            ++next;
        }
        write_json(root / page_name(pages++), rows);
    } while (next != ledger.end());
    const auto observed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    write_json(root / "snapshot.json", json::object({{"profile", Json(addon::path_to_utf8(profile))},
        {"request", Json(request_digest(request))}, {"observed", Json(observed)},
        {"pages", Json(static_cast<std::int64_t>(pages))}}));
    const auto sealed = addon::seal_usage_cache_candidate(selection.candidate_root, request.context.profile_storage->root,
        request.context.account_context, addon::random_nonce_hex(), std::string(k_format),
        [&] { return progress.cancelled(); }, &failure);
    require(sealed.has_value(), failure.reason_code);
    return {*sealed, static_cast<std::uint64_t>(observed), pages, std::nullopt};
}

void scan(const addon::Usage_scan_request& request, addon::Invocation_context& context)
{
    const auto& launch = request.context;
    const auto& environment = *launch.launch_environment;
    const auto invocation = addon::literal_launcher_invocation(launch.launcher, environment.platform_id, launch.launcher_argv);
    const auto profile = selected_profile_path(launch.account_context, launch.launcher, environment, invocation,
        launch.working_directory);
    require(profile.has_value(), "usage_profile_unavailable");
    const auto history = profile_io_path(*profile) / "session-state";
    const auto contains = [](const fs::path& parent, fs::path child) {
        if (!fs::exists(parent)) return false;
        for (;;) {
            if (fs::exists(child) && fs::equivalent(parent, child)) return true;
            if (child == child.root_path()) return false;
            child = child.parent_path();
        }
    };
    const auto verify_allocation = [&](const std::string& root) {
        const auto cache = addon::path_from_utf8(root);
        require(!contains(history, cache) && !contains(cache, history), "usage_storage_scope_conflict");
    };
    verify_allocation(request.cache_selection->candidate_root);
    if (request.cache_selection->accepted) verify_allocation(request.cache_selection->accepted->root);
    Progress progress(context);
    Snapshot snapshot;
    std::uint64_t index = 0;
    if (request.snapshot_id) {
        // The cursor pins the manifest sealed before page one, including after
        // an adapter restart. Only the metadata and requested page need rehashing.
        const auto& cursor = *request.cursor;
        const auto colon = cursor.find(':');
        require(colon != std::string::npos && cursor.size() - colon == 65, "usage_cursor_invalid");
        const auto parsed = std::from_chars(cursor.data(), cursor.data() + colon, index);
        require(parsed.ec == std::errc{} && std::to_string(index) == cursor.substr(0, colon) && index > 0,
                "usage_cursor_invalid");
        snapshot = open_snapshot({request.cache_selection->candidate_root, *request.snapshot_id, cursor.substr(colon + 1)},
            request, *profile, true, progress);
        require(index < snapshot.page_count, "usage_cursor_invalid");
    }
    else {
        snapshot = create_snapshot(request, *profile, progress);
    }
    addon::Usage_scan_page page;
    page.scope.observed_at_ms = snapshot.observed_at;
    page.scope.fresh_for_ms = 60000;
    page.account_context = launch.account_context;
    if (page.account_context) page.scope.source_account_key = page.account_context->adapter_account_key;
    page.profile_storage = launch.profile_storage;
    page.cache_candidate = snapshot.reference;
    page.snapshot_id = snapshot.reference.snapshot_id;
    page.complete = index + 1 == snapshot.page_count;
    if (!page.complete) page.next_cursor = std::to_string(index + 1) + ":" + snapshot.reference.manifest_sha256_hex;
    page.incomplete_historical_coverage = true;
    std::size_t encoded_bytes = 0;
    if (snapshot.manifest) {
        require(addon::validate_usage_cache_file(snapshot.reference, *snapshot.manifest, page_name(index),
            [&] { return progress.cancelled(); }, nullptr), "usage_cache_invalid");
    }
    for (const auto& receipt : load_page(snapshot, index)) {
        require(addon::append_usage_page_record(page, receipt.record, encoded_bytes), "usage_page_too_large");
    }
    context.succeed(addon::encode_usage_scan_result(page));
}

} // namespace

void scan_usage(addon::Invocation_context& context)
{
    if (context.request().operation != addon::k_usage_scan_operation) {
        context.reject_unknown_operation();
        return;
    }
    addon::Contract_failure failure;
    const auto request = addon::decode_usage_scan_request(context.request().payload, &failure);
    if (!request) {
        context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
        return;
    }
    try {
        scan(*request, context);
    }
    catch (const Scan_error& error) {
        context.fail(context.cancel_requested() ? addon::Result_status::CANCELLED : addon::Result_status::FAILED,
            error.what());
    }
    catch (const std::exception&) {
        context.fail(addon::Result_status::FAILED, "usage_source_unavailable");
    }
}

} // namespace copilot
