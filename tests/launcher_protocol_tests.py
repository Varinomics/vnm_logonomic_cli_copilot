"""Exercise the packaged adapter through the SDK's documented wire contracts."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


class Adapter:
    def __init__(self, executable):
        self.process = subprocess.Popen(
            [executable], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        self.request_number = 0
        self.send({
            "type": "hello", "core_protocol_generations": [1],
            "package_id": "logonomic.cli.copilot",
            "generation_digest": "a" * 64, "generation_nonce": "b" * 32,
            "limits": {
                "max_frame_bytes": 4194304, "max_json_depth": 32,
                "max_in_flight_requests": 64, "max_progress_frames_per_request": 1000,
            },
            "host": {"product": "logonomic", "version": "test"},
        })
        hello = self.receive()
        check(hello["type"] == "hello", "adapter handshake")
        check(hello["package_id"] == "logonomic.cli.copilot", "package identity")
        check(self.receive()["type"] == "capabilities", "capability negotiation")

    def send(self, value):
        # The protocol requires UTF-8 followed by LF on every platform; text
        # streams translate the delimiter to CRLF on Windows.
        frame = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.process.stdin.write(frame + b"\n")
        self.process.stdin.flush()

    def receive(self):
        line = self.process.stdout.readline()
        check(bool(line), "adapter produced an unexpected EOF")
        check(line.endswith(b"\n") and not line.endswith(b"\r\n"), "wire frame uses LF")
        return json.loads(line.decode("utf-8"))

    def invoke(self, capability, version, operation, payload, deadline_ms=5000):
        self.request_number += 1
        request_id = f"r{self.request_number}"
        self.send({
            "type": "invoke", "request_id": request_id,
            "capability_id": capability, "version": version,
            "operation": operation, "payload": payload, "deadline_ms": deadline_ms,
        })
        while True:
            reply = self.receive()
            check(reply["request_id"] == request_id, "request correlation")
            if reply["type"] == "result":
                return reply

    def close(self):
        self.send({"type": "shutdown", "grace_ms": 5000})
        self.process.stdin.close()
        check(self.process.wait(timeout=10) == 0, "clean adapter shutdown")
        errors = self.process.stderr.read().decode("utf-8")
        check(not errors, f"unexpected adapter diagnostics: {errors}")


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def successful(reply):
    check(reply.get("status") == "success", f"operation failed: {reply}")
    return reply["payload"]


def launcher(script, runner="direct", environment=""):
    return {"script_body": script, "runner_key": runner, "environment_text": environment}


def main():
    adapter = Adapter(sys.argv[1])
    platform = sys.argv[2]
    with tempfile.TemporaryDirectory(prefix="copilot-launchers-") as directory:
        # An executable fixture proves discovery without starting a provider or
        # needing credentials; native Copilot is never invoked by this test.
        program = Path(directory) / ("copilot.exe" if os.name == "nt" else "copilot")
        program.write_bytes(b"fixture")
        program.chmod(0o755)
        ambient = {"PATH": directory, "PATHEXT": ".EXE;.CMD"}
        discovered = successful(adapter.invoke("launcher-discovery", 1, "discover", {
            "ambient_environment": ambient,
            "default_home_fallback": directory, "platform": platform,
        }))
        check(len(discovered["candidates"]) == 1, "installed Copilot discovery")
        candidate = discovered["candidates"][0]
        check(candidate["script_body"] == "copilot", "discovered executable")
        check(candidate["display_name"] == "GitHub Copilot CLI", "provider presentation")

        missing = successful(adapter.invoke("launcher-discovery", 1, "discover", {
            "ambient_environment": {"PATH": str(Path(directory) / "missing")},
            "default_home_fallback": directory, "platform": platform,
        }))
        check(missing["candidates"] == [], "missing executable is not discovered")

        for script, runner, expected in [
            ("copilot", "direct", True),
            ('copilot --model "claude-sonnet-4.5"', "direct", True),
            ("gh copilot suggest", "direct", False),
            ("other-cli", "direct", False),
            ("copilot && echo changed", "bash", False),
        ]:
            recognized = successful(adapter.invoke("launcher-discovery", 1, "recognize", {
                "launcher": launcher(script, runner),
                "ambient_environment": ambient, "platform": platform,
            }))
            check(recognized["recognized"] == expected, f"recognition: {script}")

        request = {
            "launcher": launcher("copilot --model claude-sonnet-4.5", environment="COPILOT_HOME=custom"),
            "launcher_argv": ["copilot", "--model", "claude-sonnet-4.5"],
            "launch_environment": {
                "platform": platform,
                "effective_environment": {"PATH": directory, "COPILOT_HOME": "custom"},
            },
            "working_directory": directory, "title_instruction": "", "declared_assets": [],
        }
        prepared = successful(adapter.invoke("launch-preparation", 3, "prepare", request))
        check(prepared["disposition"] == {
            "version": 1, "prepared": True, "lifecycle_events": False, "coordination_delivery": False,
        }, "launch authority is limited to implemented features")
        check(prepared["environment"] == {"set": {}, "unset": []}, "environment is preserved")
        check("argv" not in prepared, "selected command is preserved")

        request["launcher_argv"] = ["another-program"]
        mismatched = successful(adapter.invoke("launch-preparation", 3, "prepare", request))
        check(not mismatched["disposition"]["prepared"], "mismatched host invocation stays unmanaged")
        check("argv" not in mismatched, "unmanaged launch is unchanged")

        invalid = adapter.invoke("launcher-discovery", 1, "recognize", {"launcher": {}})
        check(invalid["status"] == "error", "malformed request rejected")
        unknown = adapter.invoke("launch-preparation", 3, "unsupported_operation", {})
        check(unknown["status"] == "unavailable", "unknown operation rejected")
    adapter.close()
    print("PASS Copilot launcher protocol")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"FAIL {error}", file=sys.stderr)
        sys.exit(1)
