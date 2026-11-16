"""Exercise session preparation and native JSONL inspection through the SDK wire.

The event shapes come from Copilot's published session-events.ts and native
stdin-stream-off, stdin-provider-error and mcp-stdio captures. The host catalogs
come from logonomic_coordination_contract.cpp and logonomic_delegation_contract.cpp.
Only the public process, artifact and inspection contracts are inspected here;
continuation bytes are passed back without depending on their representation.
"""

import base64
import copy
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import sys
import tempfile
import uuid

from launcher_protocol_tests import Adapter, check, launcher, successful


CHUNK_BYTES = 64 * 1024
STREAM_BYTES = 1024 * 1024
STATE_CODE_UNITS = 2 * 1024 * 1024
EXCHANGE_BYTES = 4 * 1024 * 1024 - 512
PROMPT = '  Implement café with "quotes", $HOME and `literal`.\nKeep € and Ελληνικά.  \n'
ANSWER = 'Applied the change.\nUnicode: € Ελληνικά 😊\n'
MESSAGE_ID = "12f370cf-414b-4183-a7d6-8bd69d349ee6"
TIMESTAMP = "2026-11-15T20:38:31.149Z"
CATALOGS = {
    "coordination": ["status", "claim", "list", "release", "send", "receive", "notice_status", "fetch_notices"],
    "delegation": ["targets", "submit", "status", "result", "cancel"],
}


def encoded(data):
    return base64.b64encode(data).decode("ascii")


def chunks(data):
    return [encoded(data[offset:offset + CHUNK_BYTES]) for offset in range(0, len(data), CHUNK_BYTES)]


def unchunks(values):
    return b"".join(base64.b64decode(value, validate=True) for value in values)


def json_bytes(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def json_lines(records):
    return b"".join(json_bytes(record) + b"\n" for record in records)


def event(kind, data, **members):
    record = {
        "type": kind, "data": data, "id": str(uuid.uuid4()),
        "timestamp": TIMESTAMP, "parentId": "889aef79-d2ab-4986-8d49-1a5027ab5cbd",
    }
    record.update(members)
    return record


def response(content, api_call="chatcmpl-fixture-1", **data):
    message = {
        "messageId": str(uuid.uuid4()), "originatingMessageId": MESSAGE_ID,
        "model": "fixture-model", "content": content, "toolRequests": [],
        "apiCallId": api_call, "turnId": "0",
    }
    message.update(data)
    return event("assistant.message", message)


def native_records(session_id, prompt=PROMPT, answer=ANSWER):
    return [
        event("session.start", {
            "sessionId": session_id, "version": 1, "producer": "copilot-agent",
            "copilotVersion": "1.0.112", "startTime": TIMESTAMP, "alreadyInUse": False,
        }, parentId=None),
        event("user.message", {
            "content": prompt, "messageId": MESSAGE_ID, "hasSubmittedTask": True,
            "transformedContent": "<current_datetime>" + TIMESTAMP + "</current_datetime>\n\n" + prompt,
            "delivery": "idle", "turnId": "0",
        }),
        response(answer),
        {"type": "result", "timestamp": TIMESTAMP, "sessionId": session_id, "exitCode": 0},
    ]


def set_arguments(request, arguments):
    request["launcher_argv"] = arguments
    request["launcher"]["script_body"] = " ".join('"' + argument + '"' for argument in arguments)


def request_for(program, directory, platform):
    environment = {
        "PATH": str(program.parent), "COPILOT_HOME": str(Path(directory) / "native-profile"),
        "COPILOT_FIXTURE_MODE": "", "COPILOT_SESSION_TEST_VALUE": "frozen value ü",
    }
    for name in ("SystemRoot", "TEMP", "TMP"):
        if name in os.environ:
            environment[name] = os.environ[name]
    request = {
        "purpose": "implementation", "launcher": launcher(""),
        "launch_environment": {"platform": platform, "effective_environment": environment},
        "working_directory": directory, "prompt_base64": encoded(PROMPT.encode("utf-8")),
        "declared_assets": [],
    }
    set_arguments(request, [
        str(program), "--model", "fixture-model", "--reasoning-effort=high",
        "--deny-tool", "write", "shell(rm *)", "--allow-tool=shell(git status)",
        "--allow-url=https://docs.example.invalid", "--deny-url=https://private.example.invalid",
        "--add-dir", str(Path(directory) / "shared files ü"),
        "--additional-mcp-config=@" + str(Path(directory) / "saved-mcp.json"),
    ])
    return request


def delegated_request(request, directory):
    delegated = copy.deepcopy(request)
    artifact_root = Path(directory) / "task-private"
    delegated["control"] = {
        "version": 1, "task_id": "task-copilot-implementation",
        "servers": [
            {"server_id": server_id, "program": str(Path(directory) / (server_id + "-original")),
             "actions": actions, "environment": {}}
            for server_id, actions in CATALOGS.items()
        ],
        "private_artifact_root": str(artifact_root),
        "connection": {
            "version": 1, "relay_program": str(Path(directory) / "host-relay"),
            "descriptor_path": str(artifact_root / ".logonomic" / "task-servers.json"),
        },
    }
    return delegated


def prepare(adapter, request, capability="structured-session"):
    return successful(adapter.invoke(capability, 3, "prepare", request))


def refused(adapter, request, label, capability="structured-session", reason=None):
    reply = adapter.invoke(capability, 3, "prepare", request)
    check(reply["status"] == "unavailable", label + ": " + str(reply))
    if reason is not None:
        check(reply.get("reason") == reason, label + ": reason")


def invalid_prepare(adapter, request, label, capability="structured-session", reason=None):
    reply = adapter.invoke(capability, 3, "prepare", request)
    check(reply["status"] == "error", label + ": " + str(reply))
    if reason is not None:
        check(reply.get("reason") == reason, label + ": reason")


def added_options(prepared, request):
    saved = request["launcher_argv"][1:]
    arguments = prepared["process"]["argv"]
    check(arguments[:len(saved)] == saved, "saved model, permission, path, URL and configuration arguments stay exact")
    remaining = iter(arguments[len(saved):])
    options = []
    for argument in remaining:
        option, separator, value = argument.partition("=")
        if option == "--no-auto-update":
            check(not separator, "no-auto-update has no operand")
            options.append((option, ""))
        else:
            if not separator:
                value = next(remaining, None)
            check(value is not None, "injected option has an operand")
            if option == "--additional-mcp-config" and value.startswith("@"):
                value = "@" + str(Path(value[1:]))
            options.append((option, value))
    return options


def prepared_session(prepared, request, extras=()):
    process = prepared["process"]
    check(process["executable"] == {"launcher_program": True}, "host retains native process custody")
    check(process["working_directory"] == "session_workspace", "selected workspace is preserved")
    check(process["start"] == "attached", "one attached native process")
    check(process["stdin"] == "payload", "programmatic prompt uses stdin")
    check(process["stdin_payload"].encode("utf-8") == base64.b64decode(request["prompt_base64"]),
          "stdin preserves the exact prompt bytes")
    check(process["assets"] == [], "no package helper assets are needed")
    check(process["environment"] == {}, "preparation introduces no environment values")
    frozen = request["launch_environment"]["effective_environment"]
    check(set(process["environment_names"]) == set(frozen), "process receives exactly the frozen environment names")
    check(len(process["environment_names"]) == len(frozen), "environment names are unique")
    inspection = prepared["inspection"]
    check(inspection["incremental"] and not inspection["keep_stdin_open"], "stdin closes under host custody")
    check(bool(inspection["protocol_state_base64_chunks"]), "preparation seeds correlated inspection")
    options = added_options(prepared, request)
    session_ids = [value for option, value in options if option == "--session-id"]
    check(len(session_ids) == 1, "exactly one native session identity")
    session_id = session_ids[0]
    check(str(uuid.UUID(session_id)) == session_id, "native session identity is a canonical UUID")
    expected = [
        ("--session-id", session_id), ("--output-format", "json"), ("--stream", "off"),
        *extras,
    ]
    if "--no-auto-update" not in request["launcher_argv"]:
        expected.append(("--no-auto-update", ""))
    check(sorted(options) == sorted(expected), "prepared argv contains exactly the required native controls")
    return session_id


def inspect(adapter, state, stdout=b"", final=True, exit_code=0, timed_out=False, stderr=b""):
    reply = adapter.invoke("structured-session", 3, "inspect", {
        "purpose": "implementation",
        "inspection": {
            "protocol_state_base64_chunks": state, "stdout_base64_chunks": chunks(stdout),
            "stderr_base64_chunks": chunks(stderr), "final": final,
            "timed_out": timed_out, "exit_code": exit_code,
        },
    })
    result = successful(reply)
    check(len(json_bytes(result)) <= EXCHANGE_BYTES, "inspection output respects the aggregate SDK bound")
    continuation = unchunks(result["protocol_state_base64_chunks"])
    check(len(continuation.decode("utf-8").encode("utf-16-le")) // 2 <= STATE_CODE_UNITS,
          "continuation respects the SDK state bound")
    check(b"\0" not in continuation, "continuation is valid non-NUL protocol text")
    if result["complete"]:
        check(not continuation, "terminal inspection clears continuation")
        check(("result_base64_chunks" in result) != ("failure_reason" in result), "one terminal outcome")
        if "failure_reason" in result:
            check(isinstance(result["failure_reason"], str) and
                  re.fullmatch(r"[a-z][a-z0-9_]{0,63}", result["failure_reason"]),
                  "terminal failure uses the SDK consumer's stable reason-code grammar")
        check("stdin_base64_chunks" not in result and "close_stdin" not in result, "no terminal stdin action")
    else:
        check(not final, "final inspection must settle")
        check("result_base64_chunks" not in result and "failure_reason" not in result, "no premature outcome")
        check("stdin_base64_chunks" not in result, "one-shot inspection does not feed later prompts")
    return result


def initial_state(prepared):
    return prepared["inspection"]["protocol_state_base64_chunks"]


def expect_answer(result, expected, label):
    check(result["complete"] and "result_base64_chunks" in result, label + ": " + str(result))
    check(unchunks(result["result_base64_chunks"]) == expected.encode("utf-8"), label + ": exact answer bytes")


def expect_failure(adapter, prepared, records, label, **facts):
    data = records if isinstance(records, bytes) else json_lines(records)
    result = inspect(adapter, initial_state(prepared), data, **facts)
    check(result["complete"] and "failure_reason" in result, label + ": " + str(result))


def preparation_tests(adapter, request, program, directory):
    prepared = prepare(adapter, request)
    session_id = prepared_session(prepared, request)
    second = prepare(adapter, request)
    check(prepared_session(second, request) != session_id, "each prepared run gets a fresh session")

    named = copy.deepcopy(request)
    set_arguments(named, [program.name] + request["launcher_argv"][1:])
    prepared_session(prepare(adapter, named), named)
    frozen = copy.deepcopy(request)
    frozen["launch_environment"]["effective_environment"]["COPILOT_FIXTURE_MODE"] = "session-missing"
    refused(adapter, frozen, "help probes use the frozen environment")

    profile = copy.deepcopy(request)
    profile["profile_storage"] = {"version": 1, "root": str(Path(directory) / "host-profile")}
    check(prepare(adapter, profile)["profile_storage"] == profile["profile_storage"], "exact profile-storage echo")
    no_update = copy.deepcopy(request)
    set_arguments(no_update, request["launcher_argv"] + ["--no-auto-update", "--deny-tool=*"])
    prepared_session(prepare(adapter, no_update), no_update)

    unsupported = [
        ["--resume"], ["--continue"], ["--session-id", str(uuid.uuid4())], ["--agent", "custom"],
        ["-p", "another prompt"], ["-i", "another prompt"], ["login"], ["--"],
        ["--autopilot"], ["--mode=plan"], ["--fleet"], ["--available-tools=read"],
        ["--excluded-tools=write"], ["--disable-mcp-server=coordination"],
        ["--enable-mcp-server=saved"], ["-C", directory], ["--output-format=text"],
        ["--stream=on"], ["--trust-folder"], ["--new-unknown-mode"],
    ]
    for arguments in unsupported:
        invalid = copy.deepcopy(request)
        set_arguments(invalid, [str(program)] + arguments)
        refused(adapter, invalid, "unsupported task semantics: " + arguments[0])

    for capability in ("structured-session", "delegated-session"):
        review = delegated_request(request, directory) if capability == "delegated-session" else copy.deepcopy(request)
        review["purpose"] = "read_only_review"
        refused(adapter, review, "only implementation is declared", capability, "purpose_unavailable")
    review_inspection = adapter.invoke("structured-session", 3, "inspect", {
        "purpose": "read_only_review",
        "inspection": {
            "protocol_state_base64_chunks": initial_state(prepared), "stdout_base64_chunks": [],
            "stderr_base64_chunks": [], "final": True, "timed_out": False, "exit_code": 0,
        },
    })
    check(review_inspection["status"] == "unavailable" and review_inspection.get("reason") == "purpose_unavailable",
          "inspection also requires the declared implementation purpose")

    shell = copy.deepcopy(request)
    shell["launcher"]["runner_key"] = "bash"
    shell["launcher_argv"] = ["bash", "-c", shell["launcher"]["script_body"]]
    refused(adapter, shell, "shell layers cannot attest a native session")
    mismatched = copy.deepcopy(request)
    mismatched["launcher_argv"] = [str(program), "--allow-all"]
    refused(adapter, mismatched, "host argv must match the literal saved launcher")

    wrapper_directory = Path(directory) / "wrapper"
    wrapper_directory.mkdir()
    wrapper = wrapper_directory / program.name
    wrapper.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    wrapper.chmod(0o755)
    wrapped = copy.deepcopy(request)
    set_arguments(wrapped, [str(wrapper)])
    refused(adapter, wrapped, "script wrapper is not a native Copilot image")

    for prompt in (b"", b" \t\n", b"/review", b" \n/review fix this", b"invalid\xff", b"text\0text"):
        invalid = copy.deepcopy(request)
        invalid["prompt_base64"] = encoded(prompt)
        reply = adapter.invoke("structured-session", 3, "prepare", invalid)
        check(reply["status"] in ("unavailable", "error"), "unsupported native prompt is refused")
    for prompt in ("!echo is literal programmatic input", "--help is literal programmatic input"):
        literal = copy.deepcopy(request)
        literal["prompt_base64"] = encoded(prompt.encode("utf-8"))
        prepared_session(prepare(adapter, literal), literal)

    maximum = copy.deepcopy(request)
    maximum["prompt_base64"] = encoded(b"p" * (48 * 1024))
    prepared_session(prepare(adapter, maximum), maximum)
    maximum["prompt_base64"] = encoded(b"p" * (48 * 1024 + 1))
    invalid_prepare(adapter, maximum, "SDK prompt bound is explicit", reason="payload_too_large")
    excessive = copy.deepcopy(request)
    set_arguments(excessive, [str(program)] + ["--no-color"] * 63)
    refused(adapter, excessive, "injected argv must fit the SDK count bound")
    return prepared, session_id


def mode_preparation_tests(adapter, request, directory):
    for capability in ("structured-session", "delegated-session"):
        original = delegated_request(request, directory) if capability == "delegated-session" else request
        for value in ("1", "true", "yes", "on", "TRUE", " true \t", "\u00a0on\ufeff"):
            implicit = copy.deepcopy(original)
            implicit["launch_environment"]["effective_environment"]["COPILOT_PLAN_THEN_AUTOPILOT"] = value
            refused(adapter, implicit, "native plan-then-autopilot environment: " + repr(value), capability)
            for mode in (["--mode", "interactive"], ["--mode=interactive"]):
                explicit = copy.deepcopy(implicit)
                set_arguments(explicit, original["launcher_argv"] + mode)
                prepared = prepare(adapter, explicit, capability)
                saved = explicit["launcher_argv"][1:]
                check(prepared["process"]["argv"][:len(saved)] == saved,
                      "explicit interactive mode overrides the native environment without changing saved arguments")
                check(prepared["process"]["environment"] == {}, "native mode environment remains unchanged")
        for value in ("", "0", "false", "y"):
            disabled = copy.deepcopy(original)
            disabled["launch_environment"]["effective_environment"]["COPILOT_PLAN_THEN_AUTOPILOT"] = value
            prepare(adapter, disabled, capability)


def stream_tests(adapter, prepared, session_id):
    records = native_records(session_id)
    data = json_lines(records)
    expect_answer(inspect(adapter, initial_state(prepared), data), ANSWER, "native complete response")
    expect_answer(inspect(adapter, initial_state(prepared), data[:-1]), ANSWER, "valid final JSON tail")

    # Both inspection calls and SDK base64 chunks may bisect a UTF-8 sequence.
    euro = data.index("€".encode("utf-8"))
    cuts = sorted({1, 17, euro + 1, euro + 2, len(data) // 2, len(data) - 1, len(data)})
    state = initial_state(prepared)
    offset = 0
    for end in cuts:
        result = inspect(adapter, state, data[offset:end], final=False)
        check(not result["complete"], "native result cannot authenticate success before process exit")
        state = result["protocol_state_base64_chunks"]
        offset = end
    expect_answer(inspect(adapter, state), ANSWER, "arbitrary inspection boundaries")

    large = response("€")
    prefix_size = json_lines([large]).index("€".encode("utf-8"))
    large["data"]["content"] = "a" * (CHUNK_BYTES - 1 - prefix_size) + "€\ncomplete"
    prefix = inspect(adapter, initial_state(prepared), json_lines(records[:2]), final=False)
    expect_answer(inspect(adapter, prefix["protocol_state_base64_chunks"], json_lines([large, records[-1]])),
                  large["data"]["content"], "UTF-8 across the SDK chunk boundary")

    future = copy.deepcopy(records)
    future[0]["data"]["copilotVersion"] = "9.9.9"
    for record in future:
        record["futureMetadata"] = {"detail": "advisory"}
    future.insert(2, event("session.future_advisory", {"detail": "unknown native event"}))
    expect_answer(inspect(adapter, initial_state(prepared), json_lines(future)), ANSWER,
                  "unknown advisory fields and newer native versions remain compatible")

    child_start = copy.deepcopy(records[0])
    child_start["agentId"] = "child-agent"
    child_start["data"]["sessionId"] = str(uuid.uuid4())
    child = [
        child_start,
        event("user.message", {"content": "child prompt", "messageId": "child-message"}, agentId="child-agent"),
        event("session.error", {"errorType": "query", "message": "child failed"}, agentId="child-agent"),
        response("child answer", originatingMessageId="child-message"),
        response("child answer", originatingMessageId="child-message", parentToolCallId="child-tool"),
    ]
    child[3]["agentId"] = "child-agent"
    isolated = records[:3] + child + records[3:]
    expect_answer(inspect(adapter, initial_state(prepared), json_lines(isolated)), ANSWER,
                  "root and subagent isolation")
    expect_failure(adapter, prepared, records[:2] + child + records[3:],
                   "subagent response cannot complete the root")

    cases = [
        ("missing session start", records[1:]),
        ("duplicate session start", records[:1] + records),
        ("missing root prompt", [records[0], *records[2:]]),
        ("duplicate root prompt", records[:2] + records[1:]),
        ("missing root answer", records[:2] + records[3:]),
        ("duplicate root answer", records[:3] + records[2:]),
        ("answer before prompt", [records[0], records[2], records[1], records[3]]),
        ("missing native terminal", records[:-1]),
        ("duplicate native terminal", records + records[-1:]),
        ("native terminal before answer", [records[0], records[1], records[3], records[2]]),
    ]
    modifications = [
        ("wrong native session", 0, "sessionId", str(uuid.uuid4())),
        ("session already in use", 0, "alreadyInUse", True),
        ("wrong acknowledged prompt", 1, "content", "another prompt"),
        ("wrong originating message", 2, "originatingMessageId", "another-message"),
    ]
    for label, index, key, value in modifications:
        changed = copy.deepcopy(records)
        changed[index]["data"][key] = value
        cases.append((label, changed))
    for label, index, key in (
        ("missing prompt identity", 1, "messageId"),
        ("missing answer origin", 2, "originatingMessageId"),
    ):
        changed = copy.deepcopy(records)
        del changed[index]["data"][key]
        cases.append((label, changed))
    for label, key, value in (
        ("wrong terminal session", "sessionId", str(uuid.uuid4())),
        ("native terminal failure", "exitCode", 1),
    ):
        changed = copy.deepcopy(records)
        changed[-1][key] = value
        cases.append((label, changed))
    error = event("session.error", {
        "errorType": "query", "message": "400 Synthetic fixture provider failure", "statusCode": 400,
        "providerErrorType": "invalid_request_error", "providerErrorCode": "fixture_failure",
        "originatingMessageId": MESSAGE_ID,
    })
    cases.append(("provider error despite successful exits", records[:-1] + [error, records[-1]]))
    for label, changed in cases:
        expect_failure(adapter, prepared, changed, label)
    for label, tail in (
        ("truncated final record", data[:-3]),
        ("invalid trailing JSON", data + b"{\"type\":"),
        ("non-object record", data + b"[]\n"),
        ("malformed record", data + b'{"data":{}}\n'),
        ("invalid UTF-8 record", data + b"\xff\n"),
        ("NUL record", data + b"\0\n"),
    ):
        expect_failure(adapter, prepared, tail, label)
    expect_failure(adapter, prepared, records, "process failure after native success", exit_code=3)
    expect_failure(adapter, prepared, records, "timeout after native success", timed_out=True)


def response_chunk_tests(adapter, prepared, session_id):
    records = native_records(session_id)
    fragments = [
        response("first €", "chatcmpl-fragments", chunkCount=3, chunkIndex=0),
        response("", "chatcmpl-fragments", chunkCount=3, chunkIndex=1),
        response("last\n😊", "chatcmpl-fragments", chunkCount=3, chunkIndex=2),
    ]
    state = initial_state(prepared)
    for batch in (records[:3], fragments[:1], fragments[1:]):
        result = inspect(adapter, state, json_lines(batch), final=False)
        check(not result["complete"], "complete fragments still await process exit")
        state = result["protocol_state_base64_chunks"]
    expect_answer(inspect(adapter, state, json_lines(records[-1:])), "first €last\n😊",
                  "all content fragments form the latest complete root answer")

    broken = [
        ("missing final fragment", fragments[:-1]),
        ("out-of-order fragments", [fragments[1], fragments[0], fragments[2]]),
        ("repeated fragment", fragments[:1] + fragments),
        ("repeated completed group", fragments + fragments),
        ("incomplete group replaced by another response", fragments[:1] + [response("replacement", "other-call")]),
    ]
    for label, index, key, value in (
        ("conflicting fragment count", 1, "chunkCount", 4),
        ("conflicting fragment call", 1, "apiCallId", "other-call"),
        ("zero fragment count", 0, "chunkCount", 0),
        ("negative fragment index", 0, "chunkIndex", -1),
    ):
        changed = copy.deepcopy(fragments)
        changed[index]["data"][key] = value
        broken.append((label, changed))
    for key in ("apiCallId", "chunkCount", "chunkIndex"):
        changed = copy.deepcopy(fragments)
        del changed[0]["data"][key]
        broken.append(("missing chunk correlation: " + key, changed))
    for label, changed in broken:
        expect_failure(adapter, prepared, records[:3] + changed + records[3:], label)


def delegated_preparation_tests(adapter, request, directory):
    delegated = delegated_request(request, directory)
    prepared = prepare(adapter, delegated, "delegated-session")
    control = delegated["control"]
    acknowledgment = prepared["acknowledgment"]
    check(set(acknowledgment) == {"version", "server_ids", "connection"} and
          acknowledgment["version"] == 1 and acknowledgment["connection"] == {"version": 1},
          "exact host connection acknowledgment")
    check(set(acknowledgment["server_ids"]) == set(CATALOGS) and
          len(acknowledgment["server_ids"]) == len(CATALOGS), "exact ordinary host-server set")
    artifacts = prepared["private_artifacts"]
    check(len(artifacts) == 1, "one private native MCP configuration")
    artifact = artifacts[0]
    relative = PurePosixPath(artifact["relative_path"])
    parts = artifact["relative_path"].split("/")
    check(not relative.is_absolute() and parts[0] != ".logonomic" and
          all(part not in ("", ".", "..") for part in parts),
          "private artifact uses the adapter namespace beneath the supplied root")
    check("\\" not in artifact["relative_path"], "private artifact uses SDK relative path syntax")
    content = base64.b64decode(artifact["content_base64"], validate=True)
    check(len(content) <= STREAM_BYTES, "private artifact stays within the SDK content bound")
    config = json.loads(content)
    check(set(config) == {"mcpServers"}, "native MCP config has its documented root")
    check(len(config["mcpServers"]) == len(CATALOGS), "both ordinary host catalogs are configured")
    names = {}
    grants = []
    for name, server in config["mcpServers"].items():
        check(re.fullmatch(r"[A-Za-z0-9_-]+", name), "config identity survives native name sanitization")
        server_id = server["args"][-1]
        check(server_id in CATALOGS and server_id not in names, "one relay route per requested server")
        names[server_id] = name
        check(server == {
            "type": "local", "command": control["connection"]["relay_program"],
            "args": ["--descriptor", control["connection"]["descriptor_path"], "--server", server_id],
            "tools": CATALOGS[server_id], "deferTools": "never", "disableToolCache": True,
        }, "exact relay arguments, requested actions and fresh native discovery")
        for action in CATALOGS[server_id]:
            check(len(name + "-" + action) <= 64, "combined native tool identity is representable")
            grants.append(("--allow-tool", name + "(" + action + ")"))
    path = str(Path(control["private_artifact_root"]).joinpath(*relative.parts))
    session_id = prepared_session(prepared, delegated, [("--additional-mcp-config", "@" + path), *grants])
    check(not Path(control["private_artifact_root"]).exists(), "prepare leaves artifact materialization to the host")
    second = prepare(adapter, delegated, "delegated-session")
    second_config = json.loads(base64.b64decode(second["private_artifacts"][0]["content_base64"]))
    check(set(second_config["mcpServers"]).isdisjoint(config["mcpServers"]), "injected native names are per-run unique")

    missing = copy.deepcopy(delegated)
    del missing["control"]["connection"]
    refused(adapter, missing, "delegation requires the host relay connection", "delegated-session")
    unknown = copy.deepcopy(delegated)
    unknown["control"]["connection"]["version"] = 2
    invalid_prepare(adapter, unknown, "unknown connection version", "delegated-session", "unsupported_version")
    credentials = copy.deepcopy(delegated)
    credentials["control"]["servers"][0]["environment"] = {"TOKEN": "synthetic-fixture-value"}
    invalid_prepare(adapter, credentials, "connected server environment must be host-redacted", "delegated-session")
    for action in ("task/read", "écrire", "a" * 64):
        invalid = copy.deepcopy(delegated)
        invalid["control"]["servers"][0]["actions"] = [action]
        refused(adapter, invalid, "unrepresentable native tool name", "delegated-session")
    maximum = copy.deepcopy(delegated)
    original = maximum["control"]["servers"][0]
    maximum["control"]["servers"] = [
        dict(original, server_id="server-" + str(index), actions=["status"])
        for index in range(8)
    ]
    eight = prepare(adapter, maximum, "delegated-session")
    check(set(eight["acknowledgment"]["server_ids"]) == {"server-" + str(index) for index in range(8)},
          "all eight SDK server slots are acknowledged")
    maximum["control"]["servers"].append(dict(original, server_id="server-8", actions=["status"]))
    invalid_prepare(adapter, maximum, "SDK server bound is explicit", "delegated-session", "payload_too_large")
    excessive = copy.deepcopy(delegated)
    excessive["control"]["servers"] = [dict(original, actions=["action_" + str(index) for index in range(64)])]
    refused(adapter, excessive, "exact grants must fit the SDK argv bound", "delegated-session")
    return prepared, session_id, names


def readiness_events(names):
    records = []
    for server_id, name in names.items():
        records.extend([
            event("session.mcp_server_status_changed", {"serverName": name, "status": "pending"}, ephemeral=True),
            event("session.mcp_server_status_changed", {"serverName": name, "status": "connected"}, ephemeral=True),
            event("session.mcp_server_tools_loaded", {
                "serverName": name, "toolCount": len(CATALOGS[server_id]),
            }, ephemeral=True),
        ])
    records.append(event("session.mcp_servers_loaded", {
        "servers": [{"name": name, "status": "connected"} for name in names.values()],
    }, ephemeral=True))
    return records


def delegated_stream_tests(adapter, prepared, session_id, names):
    records = native_records(session_id)
    ready = readiness_events(names)
    complete = records[:1] + ready + records[1:]
    expect_answer(inspect(adapter, initial_state(prepared), json_lines(complete)), ANSWER,
                  "both ordinary catalogs ready")
    before_connected = copy.deepcopy(ready)
    for offset in (0, 3):
        before_connected[offset + 1:offset + 3] = reversed(before_connected[offset + 1:offset + 3])
    expect_answer(inspect(adapter, initial_state(prepared), json_lines(records[:1] + before_connected + records[1:])),
                  ANSWER, "fresh cached catalog may precede connected")
    unrelated = event("session.mcp_server_status_changed", {"serverName": "saved-unrelated", "status": "failed"})
    expect_answer(inspect(adapter, initial_state(prepared), json_lines(complete[:-1] + [unrelated, complete[-1]])),
                  ANSWER, "unrelated saved MCP failures do not define this task's readiness")

    expect_failure(adapter, prepared, records, "native exit zero without required readiness")
    connected_only = [item for item in ready if item["type"] != "session.mcp_server_tools_loaded"]
    expect_failure(adapter, prepared, records[:1] + connected_only + records[1:],
                   "connected does not mean tools loaded")
    expect_failure(adapter, prepared, records[:1] + ready[:3] + ready[-1:] + records[1:],
                   "one ready catalog cannot substitute for the other")
    expect_failure(adapter, prepared, records[:3] + ready + records[3:],
                   "readiness arriving after answer cannot attest it")
    child_ready = copy.deepcopy(ready)
    for item in child_ready:
        item["agentId"] = "child-agent"
    expect_failure(adapter, prepared, records[:1] + child_ready + records[1:],
                   "subagent readiness cannot attest root tools")
    for count in (0, len(CATALOGS["coordination"]) - 1, len(CATALOGS["coordination"]) + 1):
        wrong = copy.deepcopy(ready)
        wrong[2]["data"]["toolCount"] = count
        expect_failure(adapter, prepared, records[:1] + wrong + records[1:],
                       "exact requested catalog count is required")
    stale = [ready[2], ready[0], ready[1], *ready[3:]]
    expect_failure(adapter, prepared, records[:1] + stale + records[1:],
                   "tools loaded before a new pending connection are stale")
    name = names["coordination"]
    for status in ("failed", "needs-auth", "disabled", "stopped", "not_configured", "pending"):
        lost = event("session.mcp_server_status_changed", {"serverName": name, "status": status})
        expect_failure(adapter, prepared, complete[:-1] + [lost, complete[-1]], "required server loss: " + status)
    for kind in ("session.mcp_server_removed", "session.mcp_server_needs_reconnect"):
        lost = event(kind, {"serverName": name})
        expect_failure(adapter, prepared, complete[:-1] + [lost, complete[-1]], "required server loss: " + kind)
    missing = event("session.mcp_servers_loaded", {
        "servers": [{"name": names["delegation"], "status": "connected"}],
    })
    expect_failure(adapter, prepared, complete[:-1] + [missing, complete[-1]],
                   "required server disappears from native summary")


def bound_tests(adapter, prepared, session_id):
    records = native_records(session_id)
    for member, data in (
        ("stdout_base64_chunks", b"x" * (STREAM_BYTES + 1)),
        ("stderr_base64_chunks", b"x" * (CHUNK_BYTES + 1)),
        ("protocol_state_base64_chunks", b"x" * (STATE_CODE_UNITS + 1)),
    ):
        inspection = {
            "protocol_state_base64_chunks": initial_state(prepared), "stdout_base64_chunks": [],
            "stderr_base64_chunks": [], "final": True, "timed_out": False, "exit_code": 0,
        }
        inspection[member] = chunks(data)
        reply = adapter.invoke("structured-session", 3, "inspect", {
            "purpose": "implementation", "inspection": inspection,
        })
        check(reply["status"] == "error" and reply.get("reason") == "payload_too_large", "SDK member bound: " + member)

    for output_size, succeeds in ((STREAM_BYTES, True), (STREAM_BYTES + 1, False)):
        first = response("a" * (STREAM_BYTES // 2), "chatcmpl-bounded", chunkCount=2, chunkIndex=0)
        second = response("b" * (output_size - STREAM_BYTES // 2), "chatcmpl-bounded", chunkCount=2, chunkIndex=1)
        partial = inspect(adapter, initial_state(prepared), json_lines(records[:2] + [first]), final=False)
        check(not partial["complete"], "bounded output retains the incomplete response")
        result = inspect(adapter, partial["protocol_state_base64_chunks"], json_lines([second, records[-1]]))
        if succeeds:
            expect_answer(result, first["data"]["content"] + second["data"]["content"],
                          "SDK maximum result is complete")
        else:
            check(result["complete"] and "failure_reason" in result, "oversized answer fails instead of truncating")

    oversized = json_lines([event("session.future_advisory", {"content": "x" * STREAM_BYTES})])
    prefix = inspect(adapter, initial_state(prepared), json_lines(records[:-1]), final=False)
    partial = inspect(adapter, prefix["protocol_state_base64_chunks"], oversized[:STREAM_BYTES // 2], final=False)
    result = inspect(adapter, partial["protocol_state_base64_chunks"],
                     oversized[STREAM_BYTES // 2:] + json_lines(records[-1:]))
    check(result["complete"] and "failure_reason" in result,
          "oversized native record fails across inspection boundaries")


def main():
    # The supervisor's fixture mode deliberately disagrees with the frozen
    # launcher environment, so ambient probing would refuse a supported task.
    previous_mode = os.environ.get("COPILOT_FIXTURE_MODE")
    os.environ["COPILOT_FIXTURE_MODE"] = "session-missing"
    try:
        adapter = Adapter(sys.argv[1])
    finally:
        if previous_mode is None:
            del os.environ["COPILOT_FIXTURE_MODE"]
        else:
            os.environ["COPILOT_FIXTURE_MODE"] = previous_mode
    with tempfile.TemporaryDirectory(prefix="copilot-session-") as directory:
        native_directory = Path(directory) / "native copilot ü"
        native_directory.mkdir()
        program = native_directory / ("copilot.exe" if os.name == "nt" else "copilot")
        shutil.copy2(sys.argv[2], program)
        request = request_for(program, directory, sys.argv[3])
        prepared, session_id = preparation_tests(adapter, request, program, directory)
        mode_preparation_tests(adapter, request, directory)
        stream_tests(adapter, prepared, session_id)
        response_chunk_tests(adapter, prepared, session_id)
        delegated, delegated_id, names = delegated_preparation_tests(adapter, request, directory)
        delegated_stream_tests(adapter, delegated, delegated_id, names)
        bound_tests(adapter, prepared, session_id)
    adapter.close()
    print("PASS Copilot structured and delegated session contracts")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"FAIL {error}", file=sys.stderr)
        sys.exit(1)
