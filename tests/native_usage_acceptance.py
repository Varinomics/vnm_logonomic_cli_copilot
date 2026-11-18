"""Qualify official Copilot with an isolated profile and a loopback model."""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import uuid

from launcher_protocol_tests import Adapter, check
from usage_tests import all_pages, candidate, request


class Model(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.calls += 1
        identity = "copilot-accounting-" + str(self.server.calls)
        answer = {"role": "assistant", "content": "Local answer"}
        usage = {"prompt_tokens": 11, "completion_tokens": 5, "total_tokens": 16,
                 "prompt_tokens_details": {"cached_tokens": 7}}
        common = {"id": identity, "model": "local-model", "created": 1}
        if body.get("stream"):
            rows = [{**common, "object": "chat.completion.chunk",
                     "choices": [{"index": 0, "delta": delta, "finish_reason": reason}]}
                    for delta, reason in ((answer, None), ({}, "stop"))]
            if self.server.measured:
                rows[-1]["usage"] = usage
            payload = "".join("data: " + json.dumps(row) + "\n\n" for row in rows) + "data: [DONE]\n\n"
            mime = "text/event-stream"
        else:
            result = {**common, "object": "chat.completion",
                      "choices": [{"index": 0, "message": answer, "finish_reason": "stop"}]}
            if self.server.measured:
                result["usage"] = usage
            payload = json.dumps(result)
            mime = "application/json"
        data = payload.encode()
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def run(adapter, native, platform, directory, server):
    profile_path = directory / "profile"
    environment = {key: value for key, value in os.environ.items()
                   if key.upper() in {"PATH", "PATHEXT", "SYSTEMROOT", "WINDIR", "COMSPEC"}}
    environment.update(HOME=str(directory), USERPROFILE=str(directory), TEMP=str(directory), TMP=str(directory),
                       LOCALAPPDATA=str(directory / "local"), APPDATA=str(directory / "roaming"),
                       XDG_CONFIG_HOME=str(directory / "config"), XDG_CACHE_HOME=str(directory / "cache"),
                       XDG_DATA_HOME=str(directory / "data"), COPILOT_HOME=str(profile_path),
                       COPILOT_CACHE_HOME=str(directory / "cache"), COPILOT_OFFLINE="true",
                       COPILOT_PROVIDER_TYPE="openai", COPILOT_PROVIDER_BASE_URL=f"http://127.0.0.1:{server.server_port}/v1",
                       COPILOT_PROVIDER_API_KEY="loopback-fixture", COPILOT_MODEL="local-model",
                       HTTP_PROXY="http://127.0.0.1:9", HTTPS_PROXY="http://127.0.0.1:9",
                       ALL_PROXY="http://127.0.0.1:9", NO_PROXY="localhost,127.0.0.1")
    sessions = []
    for measured in (True, False):
        before = server.calls
        server.measured = measured
        session = str(uuid.uuid4())
        sessions.append(session)
        output = subprocess.run([native, "--no-auto-update", "--session-id=" + session, "--output-format=json",
                                 "--stream=off", "--disable-builtin-mcps", "--no-custom-instructions"],
                                input=b"Reply with a local answer", env=environment, cwd=directory,
                                capture_output=True, timeout=120)
        check(output.returncode == 0 and server.calls == before + 1, "one completed isolated native call")
        events = [json.loads(line) for line in
                  (profile_path / "session-state" / session / "events.jsonl").read_bytes().splitlines()]
        receipts = [event for event in events if event["type"] == "session.usage_record"]
        check(len(receipts) == 1 and receipts[0]["data"]["usage"]["accounting"]["sourceSessionId"] == session,
              "actual CLI persisted one native accounting receipt for its session")
    source = request(adapter, native, directory, platform, profile_path)
    reference, records = all_pages(adapter, candidate(source, "scan"))
    check(len(records) == 1, "a native receipt without input/output cannot establish zero usage")
    row = records[0]
    check(row["session_key"] == sessions[0] and row["input"]["value"] == 11 and row["output"]["value"] == 5
          and row["cache_read"]["value"] == 7 and row["total_tokens"] == 16, "native measured dimensions")
    check(row["model"] == "local-model" and row["aggregate_evidence"] == "complete", "native serving model")
    print(json.dumps({"native_calls": server.calls, "scanned_receipts": len(records), "tokens": row["total_tokens"],
                      "cache_read": row["cache_read"]["value"], "model": row["model"],
                      "manifest": reference["manifest_sha256_hex"]}))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--adapter", type=Path, required=True)
    parser.add_argument("--copilot", type=Path, required=True)
    parser.add_argument("--platform", required=True)
    arguments = parser.parse_args()
    adapter = Adapter(str(arguments.adapter.resolve(strict=True)))
    with ThreadingHTTPServer(("127.0.0.1", 0), Model) as server:
        server.calls = 0
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            with tempfile.TemporaryDirectory(prefix="native-copilot-accounting-") as root:
                run(adapter, arguments.copilot.resolve(strict=True), arguments.platform, Path(root), server)
        finally:
            server.shutdown()
            adapter.close()


if __name__ == "__main__":
    main()
