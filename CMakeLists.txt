cmake_minimum_required(VERSION 3.20)
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

find_package(CURL REQUIRED)
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
)
target_include_directories(ovi-code-ui PUBLIC include)
target_link_libraries(ovi-code-ui PUBLIC
    ovi-code-core
    ftxui::component
)
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
