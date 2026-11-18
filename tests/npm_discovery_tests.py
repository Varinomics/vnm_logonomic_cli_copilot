"""Exercise the saved launcher from an installed official npm Copilot package."""

import os
from pathlib import Path
import sys
import tempfile

from initial_prompt_tests import request_for as prompt_request
from launcher_protocol_tests import Adapter, check, launcher, successful
from session_tests import delegated_request, request_for as task_request


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
        check(candidate["runner_key"] == "direct" and candidate["default"], "stock npm installation selects Direct")
        check(candidate["evidence"]["source"] == "npm-native-package", "selection uses the installed platform package")
        script = candidate["script_body"]
        program = Path(script[1:-1])
        check(script[0] == script[-1] == '"' and program.is_absolute() and program.is_file(), "usable saved path")
        saved = launcher(script)
        interactive = prompt_request(program, directory, platform)
        structured = task_request(program, directory, platform)
        for request in (interactive, structured):
            request["launcher"] = saved
            request["launcher_argv"] = [str(program)]
            request["launch_environment"]["effective_environment"] = environment
        patch = successful(adapter.invoke("launch-preparation", 3, "prepare", interactive, deadline_ms=15000))
        check(Path(patch["argv"][0]) == program and "initial_prompt" in patch, "native first-message delivery")
        for capability, request in (("structured-session", structured),
                                    ("delegated-session", delegated_request(structured, directory))):
            proposed = successful(adapter.invoke(capability, 3, "prepare", request, deadline_ms=15000))
            check(proposed["process"]["executable"] == {"launcher_program": True}, "saved native executable custody")
    adapter.close()
    print("PASS stock npm discovery, first-message and structured/delegated task preparation")


if __name__ == "__main__":
    main()
