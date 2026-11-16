"""Test first-message preparation with a native fixture; no model requests."""

import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

from launcher_protocol_tests import Adapter, check, launcher, successful


def request_for(program, directory, platform):
    environment = {}
    for name in ("PATH", "HOME", "USERPROFILE", "SystemRoot", "TEMP", "TMP"):
        if name in os.environ:
            environment[name] = os.environ[name]
    prompt = 'Continue the review of café, "quoted text", $HOME and `literal` at ' + str(Path(directory) / "brief.md")
    return {
        "launcher": launcher('"' + str(program) + '" --model fixture-model --deny-tool write'),
        "launcher_argv": [str(program), "--model", "fixture-model", "--deny-tool", "write"],
        "launch_environment": {"platform": platform, "effective_environment": environment},
        "working_directory": directory, "title_instruction": "", "declared_assets": [],
        "initial_prompt": {"version": 1, "text": prompt, "referenced_file": str(Path(directory) / "brief.md")},
    }


def refused(adapter, request, label):
    result = adapter.invoke("launch-preparation", 3, "prepare", request)
    check(result["status"] == "unavailable" and result["reason"] == "initial_prompt_unavailable", label)


def mode_admission(adapter, request):
    # GitHub's CLI command reference documents enabled values and explicit
    # --mode precedence. Native Copilot 1.0.112 confirms case folding and
    # ECMAScript trimming (including BOM, excluding NEL) before selection.
    # https://docs.github.com/en/copilot/reference/copilot-cli-reference/cli-command-reference
    variable = "COPILOT_PLAN_THEN_AUTOPILOT"
    for value in ("1", "true", "yes", "on", "TRUE", " true ", "\u00a0TrUe\u00a0", "\ufefftrue\ufeff"):
        enabled = copy.deepcopy(request)
        enabled["launch_environment"]["effective_environment"][variable] = value
        refused(adapter, enabled, "environment mode cannot admit an autonomous sequence: " + repr(value))

    for value in ("", "0", "false", "off", "y", "\u0085true\u0085"):
        disabled = copy.deepcopy(request)
        disabled["launch_environment"]["effective_environment"][variable] = value
        patch = successful(adapter.invoke("launch-preparation", 3, "prepare", disabled))
        check(patch["environment"] == {"set": {}, "unset": []}, "disabled mode environment is preserved")

    enabled = copy.deepcopy(request)
    enabled["launch_environment"]["effective_environment"][variable] = "true"
    for arguments in (["--mode", "interactive"], ["--mode=interactive"]):
        explicit = copy.deepcopy(enabled)
        explicit["launcher_argv"] += arguments
        explicit["launcher"]["script_body"] += " " + " ".join(arguments)
        patch = successful(adapter.invoke("launch-preparation", 3, "prepare", explicit))
        check(patch["argv"][:-2] == explicit["launcher_argv"], "explicit interactive mode is preserved")
        check(patch["environment"] == {"set": {}, "unset": []}, "explicit mode does not clear saved environment")

    for arguments in (["--mode", "plan"], ["--mode=autopilot"], ["--name", "interactive"]):
        invalid = copy.deepcopy(enabled)
        invalid["launcher_argv"] += arguments
        invalid["launcher"]["script_body"] += " " + " ".join(arguments)
        refused(adapter, invalid, "only an explicit interactive mode overrides the environment")

    ordinary = copy.deepcopy(enabled)
    del ordinary["initial_prompt"]
    successful(adapter.invoke("launch-preparation", 3, "prepare", ordinary))

    previous = os.environ.get(variable)
    os.environ[variable] = "true"
    try:
        ambient = Adapter(sys.argv[1])
    finally:
        if previous is None:
            del os.environ[variable]
        else:
            os.environ[variable] = previous
    try:
        successful(ambient.invoke("launch-preparation", 3, "prepare", request))
    finally:
        ambient.close()


def main():
    adapter = Adapter(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="copilot-prompt-") as directory:
        native_directory = Path(directory) / "native copilot ü"
        native_directory.mkdir()
        program = native_directory / ("copilot.exe" if os.name == "nt" else "copilot")
        shutil.copy2(sys.argv[2], program)
        request = request_for(program, directory, sys.argv[3])
        patch = successful(adapter.invoke("launch-preparation", 3, "prepare", request))
        check(patch["argv"] == request["launcher_argv"] + ["--no-auto-update", "--interactive=" + request["initial_prompt"]["text"]],
              "saved permission and model options survive first-message preparation")
        check(patch["initial_prompt"] == {
            "version": 1,
            "text_sha256": hashlib.sha256(request["initial_prompt"]["text"].encode("utf-8")).hexdigest(),
        }, "exact UTF-8 prompt acknowledgment")
        check(patch["disposition"] == {"version": 1, "prepared": True, "lifecycle_events": False, "coordination_delivery": False},
              "first-message support does not claim live lifecycle")
        check(patch["environment"] == {"set": {}, "unset": []}, "no permissions or configuration injected")
        actual = subprocess.run(patch["argv"], capture_output=True, check=True, timeout=5)
        check(json.loads(actual.stdout.decode("utf-8")) == patch["argv"], "native argv receives exact Unicode and quote bytes")
        mode_admission(adapter, request)

        frozen = copy.deepcopy(request)
        frozen["launcher"]["script_body"] += " --no-auto-update"
        frozen["launcher_argv"].append("--no-auto-update")
        already_frozen = successful(adapter.invoke("launch-preparation", 3, "prepare", frozen))
        check(already_frozen["argv"].count("--no-auto-update") == 1, "existing binary selection is not duplicated")

        for text in ("--help", "- bullet", "--"):
            option_like = copy.deepcopy(request)
            option_like["initial_prompt"]["text"] = text
            rendered = successful(adapter.invoke("launch-preparation", 3, "prepare", option_like))
            check(rendered["argv"][-1] == "--interactive=" + text, "option-like text remains one attached operand")

        for arguments in (["--resume"], ["--continue"], ["--session-id", "existing"], ["--agent", "custom"],
                          ["-p", "previous"], ["-i", "previous"], ["login"], ["--"], ["--new-unknown-mode"]):
            invalid = copy.deepcopy(request)
            invalid["launcher"]["script_body"] = '"' + str(program) + '" ' + " ".join(arguments)
            invalid["launcher_argv"] = [str(program)] + arguments
            refused(adapter, invalid, "ambiguous first-message semantics: " + arguments[0])

        for text in (" leading", "trailing ", "\u00a0leading", "/help", "!echo changed", "$", "?"):
            invalid = copy.deepcopy(request)
            invalid["initial_prompt"]["text"] = text
            refused(adapter, invalid, "input shortcut or trimming refused")

        for mode in ("missing", "description", "fail"):
            unsupported = copy.deepcopy(request)
            unsupported["launcher"]["environment_text"] = "COPILOT_FIXTURE_MODE=" + mode
            unsupported["launch_environment"]["effective_environment"]["COPILOT_FIXTURE_MODE"] = mode
            refused(adapter, unsupported, "help feature evidence: " + mode)

        unbounded = copy.deepcopy(request)
        unbounded["launcher_argv"] = [str(program)] + ["--no-color"] * 62
        unbounded["launcher"]["script_body"] = '"' + str(program) + '" ' + " ".join(unbounded["launcher_argv"][1:])
        refused(adapter, unbounded, "final argv count is bounded after prompt insertion")

        shell = copy.deepcopy(request)
        shell["launcher"]["runner_key"] = "bash"
        shell["launcher_argv"] = ["bash", "-c" if sys.argv[3].startswith("linux-") else "-lc", shell["launcher"]["script_body"]]
        refused(adapter, shell, "unproved shell layers cannot attest an initial prompt")

        wrapper = native_directory / ("copilot.cmd" if os.name == "nt" else "copilot")
        if wrapper == program:
            program.unlink()
        wrapper.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        wrapper.chmod(0o755)
        nonnative = request_for(wrapper, directory, sys.argv[3])
        refused(adapter, nonnative, "script wrapper cannot attest a native executable")
        if wrapper == program:
            shutil.copy2(sys.argv[2], program)

        slow = copy.deepcopy(request)
        slow["launcher"]["environment_text"] = "COPILOT_FIXTURE_MODE=slow"
        slow["launch_environment"]["effective_environment"]["COPILOT_FIXTURE_MODE"] = "slow"
        adapter.request_number += 1
        request_id = f"r{adapter.request_number}"
        adapter.send({"type": "invoke", "request_id": request_id, "capability_id": "launch-preparation",
                      "version": 3, "operation": "prepare", "payload": slow, "deadline_ms": 15000})
        time.sleep(0.1)
        started = time.monotonic()
        adapter.send({"type": "cancel", "request_id": request_id})
        cancelled = adapter.receive()
        check(cancelled["status"] == "cancelled", "probe cancellation is capability-local")
        check(time.monotonic() - started < 5, "cancelled native probe settles promptly")
    adapter.close()
    print("PASS Copilot initial-prompt protocol")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"FAIL {error}", file=sys.stderr)
        sys.exit(1)
