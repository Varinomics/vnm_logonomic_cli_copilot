cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
endif()
project(logonomic_cli_copilot VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    if(NOT TARGET logonomic_cli_addon_sdk::sdk)
        message(FATAL_ERROR "The embedding build must provide the Logonomic CLI add-on SDK.")
    endif()
else()
    set(LOGONOMIC_CLI_ADDON_SDK_SOURCE "" CACHE PATH "Local Logonomic CLI add-on SDK source override")
    set(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(LOGONOMIC_CLI_ADDON_SDK_SOURCE)
        add_subdirectory("${LOGONOMIC_CLI_ADDON_SDK_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/sdk")
    else()
        include(FetchContent)
        FetchContent_Declare(logonomic_sdk_source
            GIT_REPOSITORY https://github.com/Varinomics/logonomic.git
            GIT_TAG master
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR sdk/logonomic_cli_addon_sdk)
        FetchContent_MakeAvailable(logonomic_sdk_source)
    endif()
endif()

function(copilot_target target)
    target_link_libraries(${target} PRIVATE logonomic_cli_addon_sdk::sdk)
    if(WIN32)
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

add_library(copilot_provider STATIC
    src/copilot_accounts.cpp src/copilot_launcher.cpp src/copilot_initial_prompt.cpp
    src/copilot_npm_native.cpp src/copilot_session.cpp src/copilot_usage.cpp src/copilot_usage_journal.cpp)
if(WIN32)
    target_link_libraries(copilot_provider PRIVATE shell32 ole32 uuid)
endif()
target_include_directories(copilot_provider PUBLIC src)
target_link_libraries(copilot_provider PUBLIC logonomic_cli_addon_sdk::sdk)
copilot_target(copilot_provider)
add_executable(copilot_adapter src/adapter.cpp)
target_link_libraries(copilot_adapter PRIVATE copilot_provider)
copilot_target(copilot_adapter)

logonomic_cli_addon_package_platform(COPILOT_PLATFORM)
set(COPILOT_EXECUTABLE_SUFFIX "${CMAKE_EXECUTABLE_SUFFIX}")
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
logonomic_cli_addon_add_package(copilot_package logonomic.cli.copilot COPILOT_PACKAGE_DIRECTORY
    ADAPTER copilot_adapter)

if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    return()
endif()

enable_testing()
add_test(NAME copilot_package_validation COMMAND logonomic_cli_addon_validate
    "${COPILOT_PACKAGE_DIRECTORY}" --platform "${COPILOT_PLATFORM}")
add_test(NAME copilot_package_conformance COMMAND logonomic_cli_addon_conformance
    "${COPILOT_PACKAGE_DIRECTORY}")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_executable(copilot_native_fixture tests/copilot_fixture.cpp)
copilot_target(copilot_native_fixture)
if(MINGW)
    target_link_options(copilot_native_fixture PRIVATE -municode)
endif()
add_test(NAME copilot_launcher_protocol COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/launcher_protocol_tests.py"
    "$<TARGET_FILE:copilot_adapter>" "${COPILOT_PLATFORM}")
add_test(NAME copilot_initial_prompt_protocol COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/initial_prompt_tests.py"
    "$<TARGET_FILE:copilot_adapter>" "$<TARGET_FILE:copilot_native_fixture>" "${COPILOT_PLATFORM}")
add_test(NAME copilot_session_protocol COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/session_tests.py"
    "$<TARGET_FILE:copilot_adapter>" "$<TARGET_FILE:copilot_native_fixture>" "${COPILOT_PLATFORM}")
add_test(NAME copilot_accounts_protocol COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/accounts_tests.py"
    "$<TARGET_FILE:copilot_adapter>" "$<TARGET_FILE:copilot_native_fixture>" "${COPILOT_PLATFORM}")
add_test(NAME copilot_usage_protocol COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/usage_tests.py"
    "$<TARGET_FILE:copilot_adapter>" "$<TARGET_FILE:copilot_native_fixture>" "${COPILOT_PLATFORM}")
set_tests_properties(copilot_usage_protocol copilot_accounts_protocol copilot_package_validation copilot_package_conformance copilot_launcher_protocol
    copilot_initial_prompt_protocol copilot_session_protocol
    PROPERTIES TIMEOUT 60 LABELS "cli-addon;provider-contract")
