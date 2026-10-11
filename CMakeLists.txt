cmake_minimum_required(VERSION 3.20)

if(CMAKE_HOST_APPLE AND NOT CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_OSX_SYSROOT)
    execute_process(
        COMMAND xcrun --sdk macosx --show-sdk-path
        OUTPUT_VARIABLE ovi_code_default_sdk
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE ovi_code_sdk_error
        RESULT_VARIABLE ovi_code_sdk_result
    )
    if(NOT ovi_code_sdk_result EQUAL 0)
        message(FATAL_ERROR "Cannot locate the macOS SDK: ${ovi_code_sdk_error}")
    endif()
    set(CMAKE_OSX_SYSROOT "${ovi_code_default_sdk}" CACHE STRING
        "macOS SDK selected by xcrun" FORCE)
endif()

project(ovi-code VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)

FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
    GIT_TAG v5.0.0
    GIT_SHALLOW TRUE
)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(json ftxui)

if(APPLE)
    # FindCURL can retain a different installation's SDK after xcode-select
    # changes. Keep system curl's headers and TAPI stub in the selected SDK.
    set(ovi_code_custom_curl FALSE)
    foreach(variable CURL_ROOT CURL_DIR CURL_INCLUDE_DIR CURL_LIBRARY
                     CURL_LIBRARY_RELEASE CURL_LIBRARY_DEBUG)
        if(${variable} AND NOT "${${variable}}" MATCHES "/SDKs/[^/]+\\.sdk(/|$)")
            set(ovi_code_custom_curl TRUE)
        endif()
    endforeach()
    if(DEFINED ENV{CURL_ROOT} AND NOT "$ENV{CURL_ROOT}" STREQUAL "")
        set(ovi_code_custom_curl TRUE)
    endif()
    if(NOT ovi_code_custom_curl)
        set(ovi_code_curl_sdk "${CMAKE_OSX_SYSROOT}")
        if(NOT IS_DIRECTORY "${ovi_code_curl_sdk}")
            set(ovi_code_sdk_name macosx)
            if(CMAKE_OSX_SYSROOT)
                set(ovi_code_sdk_name "${CMAKE_OSX_SYSROOT}")
            endif()
            execute_process(
                COMMAND xcrun --sdk "${ovi_code_sdk_name}" --show-sdk-path
                OUTPUT_VARIABLE ovi_code_curl_sdk
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_VARIABLE ovi_code_sdk_error
                RESULT_VARIABLE ovi_code_sdk_result
            )
            if(NOT ovi_code_sdk_result EQUAL 0)
                message(FATAL_ERROR "Cannot locate the macOS SDK: ${ovi_code_sdk_error}")
            endif()
        endif()
        if(NOT EXISTS "${ovi_code_curl_sdk}/usr/include/curl/curl.h" OR
           NOT EXISTS "${ovi_code_curl_sdk}/usr/lib/libcurl.tbd")
            message(FATAL_ERROR "System libcurl is missing from SDK: ${ovi_code_curl_sdk}")
        endif()
        set(CURL_INCLUDE_DIR "${ovi_code_curl_sdk}/usr/include" CACHE PATH
            "System curl headers from the selected macOS SDK" FORCE)
        foreach(variable CURL_LIBRARY CURL_LIBRARY_RELEASE CURL_LIBRARY_DEBUG)
            set(${variable} "${ovi_code_curl_sdk}/usr/lib/libcurl.tbd" CACHE FILEPATH
                "System curl library from the selected macOS SDK" FORCE)
        endforeach()
        set(CURL_NO_CURL_CMAKE TRUE)
    endif()
endif()

find_package(CURL REQUIRED)
if(APPLE AND NOT ovi_code_custom_curl AND CMAKE_OSX_SYSROOT)
    # SDK headers are supplied by -isysroot. An extra -isystem usr/include can
    # put C headers before libc++ wrappers when old implicit paths are cached.
    set_property(TARGET CURL::libcurl PROPERTY INTERFACE_INCLUDE_DIRECTORIES "")
endif()
option(OVI_CODE_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)

function(ovi_code_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
    )
    if(OVI_CODE_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
        )
    endif()
endfunction()

add_library(ovi-code-core STATIC
    src/config.cpp
    src/plugins.cpp
    src/http.cpp
    src/providers.cpp
    src/tools.cpp
    src/context.cpp
    src/metrics.cpp
    src/transcript.cpp
    src/session.cpp
    src/utf8.cpp
    src/agent_md.cpp
    src/react.cpp
)
target_include_directories(ovi-code-core PUBLIC include)
target_link_libraries(ovi-code-core PUBLIC
    nlohmann_json::nlohmann_json
    CURL::libcurl
    ${CMAKE_DL_LIBS}
)
ovi_code_warnings(ovi-code-core)

add_library(ovi-code-ui STATIC
    src/tui.cpp
    src/api_key_input.cpp
    src/clipboard.cpp
)
target_include_directories(ovi-code-ui PUBLIC include)
target_link_libraries(ovi-code-ui PUBLIC
    ovi-code-core
    ftxui::component
)
if(APPLE)
    find_library(OVI_CODE_APPLICATION_SERVICES ApplicationServices REQUIRED)
    target_link_libraries(ovi-code-ui PRIVATE ${OVI_CODE_APPLICATION_SERVICES})
endif()
ovi_code_warnings(ovi-code-ui)

add_executable(ovi-code src/main.cpp)
target_link_libraries(ovi-code PRIVATE ovi-code-ui)
ovi_code_warnings(ovi-code)

include(CTest)
if(BUILD_TESTING)
    add_executable(ovi-code-tests tests/test_main.cpp)
    target_link_libraries(ovi-code-tests PRIVATE ovi-code-core)
    ovi_code_warnings(ovi-code-tests)
    add_test(NAME unit.core COMMAND ovi-code-tests)

    add_executable(ovi-code-session-tests tests/test_session.cpp)
    target_link_libraries(ovi-code-session-tests PRIVATE ovi-code-core)
    ovi_code_warnings(ovi-code-session-tests)
    add_test(NAME unit.sessions COMMAND ovi-code-session-tests)

    add_executable(ovi-code-ui-tests tests/test_api_key_input.cpp)
    target_link_libraries(ovi-code-ui-tests PRIVATE ovi-code-ui)
    ovi_code_warnings(ovi-code-ui-tests)
    add_test(NAME unit.ui COMMAND ovi-code-ui-tests)

    add_executable(ovi-code-transcript-benchmark
        benchmarks/transcript_benchmark.cpp
    )
    target_link_libraries(ovi-code-transcript-benchmark PRIVATE ovi-code-core)
    ovi_code_warnings(ovi-code-transcript-benchmark)
    add_test(NAME benchmark.transcript
             COMMAND ovi-code-transcript-benchmark --verify)
    set_tests_properties(benchmark.transcript PROPERTIES LABELS benchmark)
endif()

include(GNUInstallDirs)
install(TARGETS ovi-code RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/ovicode/technique_plugin.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ovicode)

option(OVI_CODE_BUILD_EXAMPLE_PLUGINS "Build example technique plugins" ON)
if(OVI_CODE_BUILD_EXAMPLE_PLUGINS)
    add_library(ovi-code-deliberate MODULE plugins/examples/deliberate.cpp)
    target_include_directories(ovi-code-deliberate PRIVATE include)
    ovi_code_warnings(ovi-code-deliberate)
    set_target_properties(ovi-code-deliberate PROPERTIES
        PREFIX ""
        OUTPUT_NAME "deliberate"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    )
    install(TARGETS ovi-code-deliberate
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/ovi-code/plugins)
    if(BUILD_TESTING)
        add_test(NAME integration.bundled-plugin COMMAND ovi-code plugins)
        set_tests_properties(integration.bundled-plugin PROPERTIES
            PASS_REGULAR_EXPRESSION "deliberate.*Deliberate")
    endif()
endif()
