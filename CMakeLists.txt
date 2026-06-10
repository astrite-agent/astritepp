cmake_minimum_required(VERSION 3.28)
project(astrite LANGUAGES CXX)

# Pinned toolchain: clang 21 (apt.llvm.org). Configure with -DCMAKE_CXX_COMPILER=clang++-21.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(WARNING "astrite++ pins clang 18+; building with ${CMAKE_CXX_COMPILER_ID} is untested")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF) # headers, not named modules (DESIGN §18.1)

option(ASTRITE_SANITIZE "Build with ASan+UBSan" OFF)

add_compile_options(-Wall -Wextra -Wconversion -Wshadow -Werror -fno-rtti)
# Exceptions stay ON for now (DESIGN §18.3 open): doctest's assertion machinery and
# vendored-lib ergonomics want them in test builds; revisit for the release binary.

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

# schema + kernel; the only TU that defines the rapidyaml implementation is load.cpp.
add_library(astrite_core STATIC
    src/schema/load.cpp
    src/kernel/eval.cpp
)
target_include_directories(astrite_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(astrite_core PUBLIC third_party)

enable_testing()
add_subdirectory(tests)
