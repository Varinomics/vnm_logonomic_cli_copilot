"""Exercise the saved launcher from an installed official npm Copilot package."""

import os
from pathlib import Path
import shutil
import sys
import tempfile

from accounts_tests import profile
from initial_prompt_tests import request_for as prompt_request
from launcher_protocol_tests import Adapter, check, launcher, successful
from session_tests import delegated_request, request_for as task_request
from usage_tests import candidate as cache_candidate, scan


def javascript_profiles(adapter, candidate, directory, platform, environment):
    runner = "cmd" if os.name == "nt" else "bash"
    check(candidate["runner_key"] == runner and candidate["script_body"] == "copilot",
          "the JavaScript CLI retains its ordinary shell launcher")
    saved = launcher("copilot", runner)
    argv = ["cmd.exe", "/k", "copilot"] if os.name == "nt" else [
        "bash", "-c" if platform.startswith("linux-") else "-lc", "copilot"]
    request = {"launcher": saved, "launcher_argv": argv,
               "launch_environment": {"platform": platform, "effective_environment": environment},
               "working_directory": directory, "title_instruction": "", "declared_assets": []}
    root = Path(directory)
    # The JavaScript CLI's config.json writer ignores COPILOT_HOME.
    for parent, expected in [(None, root), ("relative", root / "relative"), (str(root / "xdg"), root / "xdg")]:
        if parent is not None:
            environment["XDG_CONFIG_HOME"] = parent
        config = expected / ".copilot"
        config.mkdir(parents=True)
        account = profile(adapter, request, root)
        check(Path(account["profile_path"]) == config and account["status"] == "available",
              "JavaScript CLI config directory from home or XDG_CONFIG_HOME")
        selected = {"version": 2, "adapter_account_key": account["adapter_account_key"],
                    "profile_path": account["profile_path"]}
        patch = successful(adapter.invoke("launch-preparation", 3, "prepare", dict(request, account_context=selected)))
        check(patch["account_context"] == selected and "argv" not in patch, "ordinary shell launch is preserved")
    store = root / "cache-store"
    store.mkdir()
    usage = dict(request, account_context=selected, profile_storage={"version": 1, "root": str(store)},
                 mode="incremental")
    del usage["title_instruction"]
    result = scan(adapter, cache_candidate(usage, "candidate"))
    check(result["status"] == "unavailable" and result["reason"] == "usage_records_unavailable",
          "older npm CLI has no native usage receipts")
    request["launcher"] = launcher("copilot --config-dir custom", runner)
    reply = adapter.invoke("accounts", 2, "discover", {
        "launcher": request["launcher"], "launch_environment": request["launch_environment"],
        "normalization_directory": directory, "default_home_fallback": directory})
    check(reply["status"] == "unavailable" and "XDG_CONFIG_HOME" in reply["message"],
          "unsupported native option explains how the JavaScript CLI selects its profile")
    reply = adapter.invoke("accounts", 2, "discover", {
        "launcher": launcher("XDG_CONFIG_HOME=custom copilot", "bash"), "launch_environment": request["launch_environment"],
        "normalization_directory": directory, "default_home_fallback": directory})
    check(reply["status"] == "unavailable" and "inline" in reply["message"] and "environment settings" in reply["message"],
          "inline Bash assignment explains where to move it")


def main():
    adapter = Adapter(sys.argv[1])
    platform = sys.argv[2]
    npm_bin = Path(sys.argv[3]).resolve()
    with tempfile.TemporaryDirectory(prefix="copilot-npm-discovery-") as directory:
        environment = {
            "PATH": str(npm_bin), "HOME": directory, "USERPROFILE": directory,
            "COPILOT_HOME": str(Path(directory) / "profile"), "COPILOT_CACHE_HOME": str(Path(directory) / "cache"),
            "LOCALAPPDATA": directory, "APPDATA": directory, "TEMP": directory, "TMP": directory,
            "COPILOT_OFFLINE": "true", "HTTPS_PROXY": "http://127.0.0.1:9",
            "HTTP_PROXY": "http://127.0.0.1:9",
        }
        if os.name == "nt":
            environment.update(SystemRoot=os.environ["SystemRoot"], PATHEXT=".EXE;.CMD")
        discovery = successful(adapter.invoke("launcher-discovery", 1, "discover", {
            "ambient_environment": {"PATH": str(npm_bin)}, "default_home_fallback": directory, "platform": platform,
        }))
        candidate, = discovery["candidates"]
        if sys.argv[4:] == ["--javascript"]:
            javascript_profiles(adapter, candidate, directory, platform, environment)
            adapter.close()
            print("PASS JavaScript npm launcher and profile discovery")
            return
        check(candidate["runner_key"] == "direct" and candidate["default"], "stock npm installation selects Direct")
        check(candidate["evidence"]["source"] == "npm-native-package", "selection uses the installed platform package")
        script = candidate["script_body"]
        program = Path(script[1:-1])
        check(script[0] == script[-1] == '"' and program.is_absolute() and program.is_file(), "usable saved path")
        if npm_bin.name == ".bin":
            workspace = Path(directory) / "linked install"
            modules = workspace / "packages" / "nested" / "node_modules"
            wrapper = modules / "@github" / "copilot"
            wrapper.mkdir(parents=True)
            for name in ("npm-loader.js", "package.json"):
                shutil.copyfile(npm_bin.parent / "@github" / "copilot" / name, wrapper / name)
            (wrapper / "npm-loader.js").chmod(0o755)
            (modules / ".bin").mkdir()
            if os.name == "nt":
                shutil.copyfile(npm_bin / "copilot.cmd", modules / ".bin" / "copilot.cmd")
            else:
                (modules / ".bin" / "copilot").symlink_to("../@github/copilot/npm-loader.js")
            exported = workspace / "node_modules" / "@github" / program.parent.name
            exported.parent.mkdir(parents=True)
            if os.name == "nt":
                exported.mkdir()
                shutil.copyfile(program.parent / "package.json", exported / "package.json")
                try:
                    os.link(program, exported / program.name)
                except OSError:
                    shutil.copyfile(program, exported / program.name)
            else:
                exported.symlink_to(program.parent, target_is_directory=True)
            discover = {
                "ambient_environment": {"PATH": str(modules / ".bin")},
                "default_home_fallback": directory, "platform": platform,
            }
            linked = successful(adapter.invoke("launcher-discovery", 1, "discover", discover))["candidates"][0]
            check(linked["script_body"] == '"' + str(exported / program.name) + '"',
                  "launcher retains the stable native export of a hoisted workspace package")
            # A nearer package shadows the outer one even if we cannot attest it.
            nearer = modules / "@github" / program.parent.name
            nearer.mkdir()
            (nearer / "package.json").write_text('{"name":"different"}', encoding="utf-8")
            blocked = successful(adapter.invoke("launcher-discovery", 1, "discover", discover))["candidates"][0]
            check(blocked["runner_key"] != "direct", "an unrecognized nearer dependency preserves the shell runner")
        saved = launcher(script)
        interactive = prompt_request(program, directory, platform)
        structured = task_request(program, directory, platform)
        for request in (interactive, structured):
            request["launcher"] = saved
            request["launcher_argv"] = [str(program)]
            request["launch_environment"]["effective_environment"] = environment
        patch = successful(adapter.invoke("launch-preparation", 3, "prepare", interactive, deadline_ms=45000))
        check(Path(patch["argv"][0]) == program and "initial_prompt" in patch, "native first-message delivery")
        for capability, request in (("structured-session", structured),
                                    ("delegated-session", delegated_request(structured, directory))):
            proposed = successful(adapter.invoke(capability, 3, "prepare", request, deadline_ms=45000))
            check(proposed["process"]["executable"] == {"launcher_program": True}, "saved native executable custody")
    adapter.close()
    print("PASS stock npm discovery, first-message and structured/delegated task preparation")


if __name__ == "__main__":
    main()
