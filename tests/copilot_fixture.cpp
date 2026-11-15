#include "logonomic_cli_addon/json.h"
#include "logonomic_cli_addon/package_source.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace addon = logonomic::cli_addon;

namespace {

int run(const std::vector<std::string>& arguments)
{
    if (arguments.size() == 3 && arguments[1] == "--no-auto-update" && arguments[2] == "--help") {
        const char* selected_mode = std::getenv("COPILOT_FIXTURE_MODE");
        const std::string mode = selected_mode ? selected_mode : "";
        if (mode == "slow") {
            std::this_thread::sleep_for(std::chrono::seconds(20));
        }
        if (mode == "fail") {
            return 2;
        }
        std::cout << "Usage: copilot [OPTIONS]\n\nOptions:\n  --no-auto-update\n";
        if (mode == "description") {
            std::cout << "    This build does not implement --interactive <prompt>.\n";
        }
        else
        if (mode != "missing") {
            std::cout << "  -i, --interactive <prompt>\n";
        }
        return 0;
    }
    addon::Json_value result = addon::Json_value::array();
    for (const auto& argument : arguments) {
        result.push_back(addon::Json_value(argument));
    }
    std::cout << addon::serialize_json(result) << '\n';
    return 0;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> arguments;
    for (int index = 0; index < argc; ++index) {
        arguments.push_back(addon::path_to_utf8(std::filesystem::path(argv[index])));
    }
    return run(arguments);
}
#else
int main(int argc, char** argv)
{
    return run(std::vector<std::string>(argv, argv + argc));
}
#endif
