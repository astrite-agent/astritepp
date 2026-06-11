cmake_minimum_required(VERSION 3.28)
project(astrite LANGUAGES CXX)

# Pinned toolchain: clang 21 (apt.llvm.org). Configure with -DCMAKE_CXX_COMPILER=clang++-21.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(WARNING "astrite++ pins clang 21; building with ${CMAKE_CXX_COMPILER_ID} is untested")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF) # headers, not named modules (DESIGN §18.1)

option(ASTRITE_SANITIZE "Build with ASan+UBSan" OFF)

# Our warning wall, scoped to our own targets (vendored trees build with their
# own flags). Exceptions stay ON for now (DESIGN §18.3 open): doctest's assertion
# machinery wants them in test builds; revisit for the release binary.
add_library(astrite_flags INTERFACE)
target_compile_options(astrite_flags INTERFACE
    -Wall -Wextra -Wconversion -Wshadow -Werror -fno-rtti)

if(ASTRITE_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Vendored third-party headers are SYSTEM includes so our warning wall stays ours.
add_library(third_party INTERFACE)
target_include_directories(third_party SYSTEM INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party/glaze
)

add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE ${CMAKE_SOURCE_DIR}/third_party/doctest)

# FTXUI (vendored, zero-dep): builds as its own targets with its own flags.
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/ftxui SYSTEM EXCLUDE_FROM_ALL)

# The enforcement floor: zero third-party deps, the only place with raw syscalls.
add_library(astrite_sandbox STATIC
    src/sandbox/sandbox.cpp
)
target_include_directories(astrite_sandbox PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(astrite_sandbox PUBLIC astrite_flags)

add_library(astrite_core STATIC
    src/schema/load.cpp
    src/kernel/eval.cpp
)
target_include_directories(astrite_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(astrite_core PUBLIC third_party astrite_flags)

# System libcurl: trust class of libc, not a vendored dependency (DESIGN §14).
find_package(CURL REQUIRED)
add_library(astrite_llm STATIC
    src/llm/client.cpp
)
target_link_libraries(astrite_llm PUBLIC astrite_core CURL::libcurl)

add_library(astrite_runtime STATIC
    src/runtime/runtime.cpp
)
target_link_libraries(astrite_runtime PUBLIC astrite_llm astrite_sandbox)

# Embed the default components at configure time. (The consteval authority fold
# upgrades this to generated constexpr IR + static_assert later in M6.)
file(READ ${CMAKE_SOURCE_DIR}/components/turn.json TURN_JSON)
file(READ ${CMAKE_SOURCE_DIR}/components/echo.json ECHO_JSON)
file(READ ${CMAKE_SOURCE_DIR}/components/fs_read.json FS_READ_JSON)
file(READ ${CMAKE_SOURCE_DIR}/components/fs_list.json FS_LIST_JSON)
file(READ ${CMAKE_SOURCE_DIR}/components/search_find.json SEARCH_FIND_JSON)
file(READ ${CMAKE_SOURCE_DIR}/components/fs_write.json FS_WRITE_JSON)
file(READ ${CMAKE_SOURCE_DIR}/components/fs_edit.json FS_EDIT_JSON)
file(READ ${CMAKE_SOURCE_DIR}/components/component_catalog.json COMPONENT_CATALOG_JSON)
file(READ ${CMAKE_SOURCE_DIR}/components/component_check.json COMPONENT_CHECK_JSON)
file(READ ${CMAKE_SOURCE_DIR}/components/component_propose.json COMPONENT_PROPOSE_JSON)
file(READ ${CMAKE_SOURCE_DIR}/components/diff_workspace.json DIFF_WORKSPACE_JSON)
file(READ ${CMAKE_SOURCE_DIR}/components/workspace_context.json WORKSPACE_CONTEXT_JSON)
configure_file(cmake/embedded_components.hpp.in generated/embedded_components.hpp @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    components/turn.json components/echo.json components/fs_read.json
    components/fs_list.json components/search_find.json components/fs_write.json
    components/fs_edit.json components/component_catalog.json components/component_check.json
    components/component_propose.json components/diff_workspace.json
    components/workspace_context.json)

add_executable(astrite src/frontends/readline/main.cpp)
target_include_directories(astrite PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(astrite PRIVATE astrite_runtime)

add_executable(astrite-tui src/frontends/tui/main.cpp)
target_include_directories(astrite-tui PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(astrite-tui PRIVATE astrite_runtime ftxui::component)

enable_testing()
add_subdirectory(tests)
