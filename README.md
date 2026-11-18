# GitHub Copilot CLI add-on for Logonomic

This repository builds `logonomic.cli.copilot`, a native Logonomic add-on for
the standalone [GitHub Copilot CLI](https://github.com/github/copilot-cli).
Install and sign in to Copilot CLI separately; the add-on uses your existing
`copilot` installation and configuration.

The package supplies launcher discovery, recognition, interactive launch
preparation, source profiles, initial-message delivery and delegated implementation tasks. Its
capability sidecars are the supported integration contract.
Copilot retains control of authentication, tool permissions and its terminal UI.

## Copilot profiles

Logonomic identifies the Copilot configuration and session directory selected
by a saved launcher. Copilot defaults to `~/.copilot`. Set `COPILOT_HOME` in
the launcher environment to select another profile, or retain a saved
`--config-dir` argument. The argument takes precedence. Task and interactive
preparation check the selected directory before acknowledging the account.
Copilot manages sign-in and credentials independently of this profile, and
`COPILOT_CACHE_HOME` selects its separate runtime cache.

## Interactive continuation

For a Logonomic continuation, select a Direct launcher pointing at the native
Copilot executable. The add-on verifies the installed executable's help for
interactive-prompt support and supplies the host's first message through
`--interactive`. It adds `--no-auto-update` for that invocation so Copilot uses
the exact installed binary that was checked. Existing model, permission and
configuration arguments retain their values; no additional file or tool
permissions are granted.

A continuation must start a fresh interactive session. Existing prompts,
resume/session selectors, commands, agent selection and arguments whose
first-message behavior is not established make this capability unavailable.
Input shortcuts and surrounding whitespace are also refused because native
input handling can interpret or trim them. Shell launchers and script wrappers
continue to work for ordinary launches; they do not attest initial-message
delivery. Feature detection does not impose a Copilot version ceiling.

## Implementation tasks

Save a Direct launcher pointing at the native Copilot executable. Discovery
also finds the platform binary of supported npm installations and saves its
stable package path. Custom wrappers keep their existing shell runner. From a live
Logonomic session, use the host-provided command to list eligible targets:

```sh
"$LOGONOMIC_VNMCTL_PATH" delegate targets
```

Use the exact target ID for the saved Copilot launcher. From the checkout where
the task should run, supply its prompt through stdin:

```sh
"$LOGONOMIC_VNMCTL_PATH" delegate run --title 'Fix parser boundary handling' \
  --target TARGET_ID < task-prompt.txt
```

The add-on starts a fresh native Copilot session with the task prompt on stdin.
A per-task model or reasoning-effort selection overrides the corresponding
saved setting for that invocation. Other model settings, tool permissions,
path and URL rules, and compatible configuration arguments carry through.
Logonomic's requested coordination and
delegation actions are attached through its task-server relay, with narrow native
allow rules for those actions. Saved deny rules still take precedence. The add-on
does not grant additional shell, file or URL access.

The installed executable must advertise the native session, JSON output, stream
and MCP configuration options. Each task disables automatic binary updates so
the probed executable supplies the session protocol. The selected launcher must
not contain another prompt, resume selector, custom agent, autonomous mode,
working-directory override, or tool/MCP selector that could hide task actions.
Unrecognized options make task preparation unavailable. Native slash-command
prompts are also refused because programmatic Copilot can route them as commands.

Task completion requires the requested session and exact prompt acknowledgment,
complete root assistant output, ready task-server catalogs, a successful native
terminal result and process exit zero. Native errors, missing task tools,
malformed or truncated output, incomplete response fragments and exceeded
exchange bounds fail the task. Hosts that negotiate structured model
observation receive the serving model reported by a root native assistant
message. Copilot retains control of authentication. Delegated tasks use the
implementation purpose.

The package does not declare live session identity, idle/turn tracking, history
resume or conversation export. Copilot can keep several sessions alive in one
terminal, and its documented hooks do not establish which one is foreground.

## Build and install

Build with CMake 3.25 or newer and a C++20 compiler. Linux packaging also
requires Python 3.11 or newer, `patchelf`, binutils, and the Debian or Ubuntu
package metadata and copyright notices for the installed compiler runtimes.
The SDK uses that metadata to record the bundled runtime's source provenance.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLOGONOMIC_CLI_ADDON_SDK_SOURCE=/path/to/logonomic/sdk/logonomic_cli_addon_sdk
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix /path/to/new/package-export
```

Without the source override, CMake fetches the SDK from Logonomic's `master`
branch, which requires access to that repository. The SDK is a standalone
standard-library implementation; building this add-on does not build Qt or the
desktop application.

With the official npm package installed, exercise its normal discovery and
native preparation using the npm bin directory containing the Copilot shim:

```sh
python tests/npm_discovery_tests.py /path/to/copilot_adapter linux-x86_64 /path/to/node_modules/.bin
```

The exported `logonomic.cli.copilot` directory is the complete package. Install
it through Logonomic's CLI add-on package flow, or put it beneath a configured
add-on development root. Use a fresh export destination for each build.

The package targets Windows x86_64, Linux x86_64 and macOS arm64. The SDK also
supports embedding this project with `LOGONOMIC_CLI_ADDON_EMBEDDED` after the
embedding project has provided `logonomic_cli_addon_sdk::sdk`.
