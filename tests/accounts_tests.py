"""Check native source profile attribution through the package protocol.

An optional installed Copilot argument also checks its real configuration
writer against the discovered paths, entirely in temporary profiles.
"""

import copy
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from initial_prompt_tests import request_for as initial_request
from launcher_protocol_tests import Adapter, check, successful
from session_tests import delegated_request, request_for, set_arguments


def profile(adapter, request, fallback):
    accounts_request = {
        "launcher": request["launcher"],
        "launch_environment": request["launch_environment"],
        "normalization_directory": request["working_directory"],
        "default_home_fallback": str(fallback),
    }
    inventory = successful(adapter.invoke("accounts", 2, "discover", accounts_request))["accounts"]
    check(len(inventory) == 1, "one native source profile selected by the launcher")
    resolved = successful(adapter.invoke("accounts", 2, "resolve", accounts_request))
    check(resolved["adapter_account_key"] == inventory[0]["adapter_account_key"], "exact admitted account")
    return inventory[0]


def source_routes(adapter, program, directory, platform, native=False):
    home = directory / "home"
    work = directory / "work"
    cache = directory / "cache"
    for path in (home, work, cache):
        path.mkdir()
    source = request_for(program, str(directory), platform)
    environment = source["launch_environment"]["effective_environment"]
    environment.update({
        "HOME": str(home), "USERPROFILE": str(home), "COPILOT_CACHE_HOME": str(cache),
        "LOCALAPPDATA": str(home / "local"), "APPDATA": str(home / "roaming"),
        "XDG_CONFIG_HOME": str(home / "xdg-config"), "XDG_STATE_HOME": str(home / "xdg-state"),
        "COPILOT_AUTO_UPDATE": "false",
    })
    cases = [
        (None, [], home / ".copilot"),
        ("from environment", [], directory / "from environment"),
        ("discarded", ["--config-dir="], directory),
        ("~/literal", [], directory / "~" / "literal"),
        ("discarded", ["--config-dir=unused", "--config-dir", "chosen ü"], directory / "chosen ü"),
        ("discarded", ["--config-dir", "~/override"], home / "override"),
        ("relative", ["-C", str(work)], work / "relative"),
        ("discarded", ["--config-dir=chosen", "-C", "work"], work / "chosen"),
    ]
    if os.name != "nt":
        target = directory / "real" / "child"
        target.mkdir(parents=True)
        (directory / "alias").symlink_to(target, target_is_directory=True)
        cases.extend([
            ("../physical", ["-C", "alias"], target.parent / "physical"),
            ("lexical-work", ["-C", "alias/.."], directory / "lexical-work"),
            ("alias/../lexical-env", [], directory / "lexical-env"),
            ("discarded", ["--config-dir=alias/../lexical-flag"], directory / "lexical-flag"),
        ])
    seen = set()
    for override, args, expected in cases:
        if override is None:
            environment.pop("COPILOT_HOME", None)
        else:
            environment["COPILOT_HOME"] = override
        set_arguments(source, [str(program), *args])
        initial_status = "available" if expected.is_dir() else "unavailable"
        account = profile(adapter, source, home)
        check(Path(account["profile_path"]) == expected, f"native profile expected at {expected}")
        check(account["display_name"] == expected.name, "native folder name")
        check(account["adapter_account_key"] not in seen, "distinct source profiles have distinct keys")
        seen.add(account["adapter_account_key"])
        check(account["status"] == initial_status, "discovery reports initial profile availability")
        if native:
            child = {k: os.environ[k] for k in ("PATH", "SystemRoot", "WINDIR", "TEMP", "TMP") if k in os.environ}
            child.update(environment)
            subprocess.run([program, "--no-auto-update", *args, "config", "footer.showQuota", "off"],
                           env=child, cwd=directory, check=True, capture_output=True, timeout=60)
            check((expected / "settings.json").is_file(), "native writer used the reported source")
        else:
            expected.mkdir(parents=True, exist_ok=True)
        current = profile(adapter, source, home)
        check(current["status"] == "available" and current["adapter_account_key"] == account["adapter_account_key"],
              "creating the profile preserves its identity")
    explicit = directory / "last profile"
    environment["COPILOT_HOME"] = str(explicit)
    set_arguments(source, [str(program)])
    first = profile(adapter, source, home)
    environment["COPILOT_CACHE_HOME"] = str(home / "another cache")
    check(profile(adapter, source, home) == first, "cache changes preserve source identity")
    environment["COPILOT_HOME"] = str(directory / "different")
    check(profile(adapter, source, home)["adapter_account_key"] != first["adapter_account_key"],
          "separate selected profile")


def selected_preparation(adapter, program, directory, platform):
    structured = request_for(program, str(directory), platform)
    initial = initial_request(program, str(directory), platform)
    initial["launch_environment"] = copy.deepcopy(structured["launch_environment"])
    plain = copy.deepcopy(initial)
    plain.pop("initial_prompt")
    delegated = delegated_request(structured, str(directory))
    for capability, request in [
        ("launch-preparation", plain), ("launch-preparation", initial),
        ("structured-session", structured), ("delegated-session", delegated),
    ]:
        source = profile(adapter, request, directory)
        for version in (1, 2):
            account = {"version": version, "adapter_account_key": source["adapter_account_key"]}
            if version == 2:
                account["profile_path"] = source["profile_path"]
            request["account_context"] = account
            result = successful(adapter.invoke(capability, 3, "prepare", request, deadline_ms=15000))
            check(result["account_context"] == account, "acknowledge the exact native profile")
            wrong = copy.deepcopy(request)
            wrong["launch_environment"]["effective_environment"]["COPILOT_HOME"] = str(directory / "other profile")
            rejected = adapter.invoke(capability, 3, "prepare", wrong)
            check(rejected["status"] == "unavailable" and rejected["reason"] == "account_profile_mismatch",
                  "a different source profile cannot fulfill this operation")
        for arg in ("--config-dir=", "--config-dir=-profile"):
            with_selector = copy.deepcopy(request)
            set_arguments(with_selector, [*request["launcher_argv"], arg])
            chosen = profile(adapter, with_selector, directory)
            with_selector["account_context"] = {
                "version": 2, "adapter_account_key": chosen["adapter_account_key"],
                "profile_path": chosen["profile_path"],
            }
            accepted = successful(adapter.invoke(capability, 3, "prepare", with_selector, deadline_ms=15000))
            check(accepted["account_context"] == with_selector["account_context"], "attached config directory")
        wrong = copy.deepcopy(request)
        wrong["account_context"]["profile_path"] = str(directory / "unrelated")
        rejected = adapter.invoke(capability, 3, "prepare", wrong)
        check(rejected["status"] == "unavailable", "a valid key cannot attest the wrong retained path")


def main():
    adapter = Adapter(sys.argv[1])
    try:
        with tempfile.TemporaryDirectory(prefix="copilot-accounts-") as temporary:
            directory = Path(temporary)
            program = directory / ("copilot.exe" if os.name == "nt" else "copilot")
            shutil.copy2(sys.argv[2], program)
            source_routes(adapter, program, directory, sys.argv[3])
            selected_preparation(adapter, program, directory, sys.argv[3])
        if len(sys.argv) == 5:
            with tempfile.TemporaryDirectory(prefix="copilot-native-profiles-") as temporary:
                source_routes(adapter, Path(sys.argv[4]).resolve(), Path(temporary), sys.argv[3], native=True)
    finally:
        adapter.close()
    print("PASS Copilot account profiles")


if __name__ == "__main__":
    main()
