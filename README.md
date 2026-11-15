# GitHub Copilot CLI add-on for Logonomic

This repository builds `logonomic.cli.copilot`, a native Logonomic add-on for
the standalone [GitHub Copilot CLI](https://github.com/github/copilot-cli).
Install and sign in to Copilot CLI separately; the add-on uses your existing
`copilot` installation and configuration.

The package supplies launcher discovery, recognition and interactive launch
preparation. Its capability sidecars are the supported integration contract.
Copilot retains control of authentication, tool permissions and its terminal UI.

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

The exported `logonomic.cli.copilot` directory is the complete package. Install
it through Logonomic's CLI add-on package flow, or put it beneath a configured
add-on development root. Use a fresh export destination for each build.

The package targets Windows x86_64, Linux x86_64 and macOS arm64. The SDK also
supports embedding this project with `LOGONOMIC_CLI_ADDON_EMBEDDED` after the
embedding project has provided `logonomic_cli_addon_sdk::sdk`.
