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

add_executable(ovi-code
    src/main.cpp
    src/config.cpp
    src/http.cpp
    src/providers.cpp
    src/tools.cpp
    src/agent_md.cpp
    src/react.cpp
    src/tui.cpp
)
target_include_directories(ovi-code PRIVATE include)
target_link_libraries(ovi-code PRIVATE
    nlohmann_json::nlohmann_json
    ftxui::component
    CURL::libcurl
)
target_compile_options(ovi-code PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
)

install(TARGETS ovi-code RUNTIME DESTINATION bin)
