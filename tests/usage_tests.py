"""Drive historical usage through the package's normal scan and account operations."""

import copy
import json
from pathlib import Path
import os
import shutil
import sys
import tempfile
import uuid

from accounts_tests import profile
from launcher_protocol_tests import Adapter, check, successful
from session_tests import request_for, set_arguments


def request(adapter, program, temporary, platform, native_profile):
    source = request_for(program, str(temporary), platform)
    set_arguments(source, [str(program)])
    del source["purpose"], source["prompt_base64"]
    source["launch_environment"]["effective_environment"]["COPILOT_HOME"] = str(native_profile)
    source["launcher"]["environment_text"] = "COPILOT_HOME=" + str(native_profile)
    selected = profile(adapter, source, native_profile)
    source["account_context"] = {"version": 2, "adapter_account_key": selected["adapter_account_key"],
                                 "profile_path": selected["profile_path"]}
    store = temporary / ("store-" + selected["adapter_account_key"])
    store.mkdir(exist_ok=True)
    source.update(profile_storage={"version": 1, "root": str(store)}, mode="incremental",
                  snapshot_id=None, cursor=None)
    return source


def candidate(source, name, accepted=None):
    result = copy.deepcopy(source)
    directory = Path(result["profile_storage"]["root"]) / name
    directory.mkdir()
    result.update(snapshot_id=None, cursor=None, cache_selection={
        "version": 1, "accepted": accepted, "candidate_root": str(directory)})
    return result


def scan(adapter, payload):
    return adapter.invoke("usage-scan", 3, "scan", payload, deadline_ms=15000)


def journal(profile_directory, identifier, records):
    path = profile_directory / "session-state" / identifier / "events.jsonl"
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as writer:
        for record in [{"type": "session.start", "data": {"sessionId": identifier}}, *records]:
            writer.write(json.dumps(record) + "\n")
    return path


def receipt(session, sequence):
    return {"type": "session.usage_record", "id": str(uuid.uuid4()),
            "timestamp": "2026-11-18T01:23:45.678Z", "data": {"usage": {
        "inputTokens": 11, "outputTokens": 5, "cacheReadTokens": 7, "cacheWriteTokens": 0,
        "cacheDetailsReported": True, "responseModel": "served", "model": "selected",
        "accounting": {"sourceSessionId": session, "sequence": sequence, "usageId": f"call-{sequence}"}}}}


def all_pages(adapter, payload):
    page = successful(scan(adapter, payload))
    ref = page["cache_candidate"]
    rows = list(page["records"])
    while not page["complete"]:
        payload = dict(payload, snapshot_id=page["snapshot_id"], cursor=page["next_cursor"])
        page = successful(scan(adapter, payload))
        check(page["cache_candidate"] == ref, "every page names the originally sealed candidate")
        rows.extend(page["records"])
    return ref, rows


def exercise(adapter, program, directory, platform):
    first = directory / "first profile"
    second = directory / "second profile"
    session, fork = str(uuid.uuid4()).upper(), str(uuid.uuid4())
    events = [receipt(session, sequence) for sequence in range(1, 66)]
    special = events[1]["data"]["usage"]
    special["cacheDetailsReported"] = False
    del special["responseModel"]
    missing = receipt(session, 99)
    del missing["data"]["usage"]["outputTokens"]
    summary = {"type": "session.shutdown", "data": {"modelMetrics": {"model": {"usage": {"inputTokens": 9000}}}}}
    original = journal(first, session, [*events, missing, summary])
    # Active JSONL logs can contain the prefix of an unpublished record.
    with original.open("ab") as writer:
        writer.write(b'{"type":"session.usage_record"')
    copyfile = journal(first, fork, [events[0]])
    source = request(adapter, program, directory, platform, first)
    invocation = candidate(source, "initial")
    with original.open("ab"):
        head = successful(scan(adapter, invocation))
    check(not head["complete"] and head["incomplete_historical_coverage"], "a limited multi-page snapshot")
    check(head["account_context"] == source["account_context"] and
          head["scope"]["source_account_key"] == source["account_context"]["adapter_account_key"], "selected account")
    original.unlink()
    copyfile.unlink()
    continuation = dict(invocation, snapshot_id=head["snapshot_id"], cursor=head["next_cursor"])
    tail = successful(scan(adapter, continuation))
    rows = head["records"] + tail["records"]
    check(tail["complete"] and tail["cache_candidate"] == head["cache_candidate"] and len(rows) == 65,
          "sealed snapshot survives deletion and fork receipts count once")
    keys = [entry["record_id"] for entry in rows]
    check(keys == sorted(set(keys)), "stable sorted record identity")
    by_id = {entry["record_id"]: entry for entry in rows}
    row = by_id["copilot:" + session + ":1"]
    check(row["total_tokens"] == 16 and row["cache_read"]["value"] == 7 and row["input"]["value"] == 11
          and row["output"]["value"] == 5, "measured input and output include cache reads once")
    check(row["model"] == "served" and row["session_key"] == session and row["completed_date"] == "2026-11-18",
          "actual serving model, native source session and UTC date")
    row2 = by_id["copilot:" + session + ":2"]
    check("model" not in row2 and "value" not in row2["cache_read"] and
          row2["cache_read"]["coverage"]["support"] == "unknown", "unreported native evidence stays unknown")
    accepted = head["cache_candidate"]
    ref, retained = all_pages(adapter, candidate(source, "retain", accepted))
    check(retained == rows, "accepted immutable receipts survive source deletion")
    changed = copy.deepcopy(events[0])
    changed["data"]["usage"]["outputTokens"] += 1
    journal(first, fork, [changed])
    conflict = scan(adapter, candidate(source, "conflict", ref))
    check(conflict.get("reason") == "usage_receipt_conflict", "a contradictory copy cannot replace accepted usage")
    journal(first, fork, [])
    reset = candidate(source, "reset")
    reset["mode"] = "reset"
    check(all_pages(adapter, reset)[1] == [], "explicit reset reconstructs exclusively from native receipts")
    journal(second, session, [events[0]])
    other = request(adapter, program, directory, platform, second)
    _, copied = all_pages(adapter, candidate(other, "copied"))
    check(copied == [row] and other["account_context"] != source["account_context"],
          "another frozen profile exposes exactly its local copy under the same cross-source event identity")
    separate = receipt(session, 1)
    separate["data"]["usage"]["accounting"]["usageId"] = "independent-call"
    journal(second, session, [separate])
    _, independent = all_pages(adapter, candidate(other, "independent"))
    check(independent[0]["capabilities"] != row["capabilities"],
          "reusing a session UUID and sequence in a fresh profile cannot correlate two distinct calls")
    wrong = candidate(other, "wrong-account")
    wrong["account_context"] = source["account_context"]
    check(scan(adapter, wrong)["status"] != "success", "a different selected native account has no scan authority")
    moved = second.rename(directory / "moved profile")
    unavailable = candidate(other, "absent-profile")
    unavailable["mode"] = "reset"
    check(scan(adapter, unavailable)["status"] != "success", "a missing profile cannot authorize zero usage on reset")
    moved.rename(second)
    invalid = dict(continuation, mode="repair")
    check(scan(adapter, invalid)["status"] != "success", "frozen continuation rejects a changed request")
    bad_cursor = dict(continuation, cursor="1:" + "0" * 64)
    check(scan(adapter, bad_cursor)["status"] != "success", "continuation must use its original sealed manifest")
    sealed_page = Path(head["cache_candidate"]["root"]) / "receipts-1.json"
    with sealed_page.open("ab") as writer:
        writer.write(b" ")
    check(scan(adapter, continuation)["status"] != "success", "a changed page cannot be read from the frozen snapshot")
    check(scan(adapter, candidate(source, "damaged-cache", head["cache_candidate"]))["status"] != "success",
          "damaged accepted inventory must not contribute to a new candidate")
    if os.name != "nt":
        (second / "session-state" / str(uuid.uuid4())).symlink_to(first / "session-state" / fork, target_is_directory=True)
        check(scan(adapter, candidate(other, "linked"))["status"] != "success", "a linked session is never scanned")
    return ref


def main():
    with tempfile.TemporaryDirectory(prefix="copilot-usage-") as root:
        directory = Path(root)
        program = directory / ("copilot.exe" if os.name == "nt" else "copilot")
        shutil.copy2(sys.argv[2], program)
        adapter = Adapter(sys.argv[1])
        try:
            exercise(adapter, program, directory, sys.argv[3])
        finally:
            adapter.close()


if __name__ == "__main__":
    main()
