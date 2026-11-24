# hamr - a test runner scripted in cluau. The native host (src/) holds only
# what a script cannot do: processes, files, clocks, JSON and the type
# checker. The standard library (std/) is cluau, embedded in the binary.
#
# Luau and cluau come from HAMR_LUAU_DIR / HAMR_CLUAU_DIR when given (a
# workspace that already vendors them), else from their repositories at the
# pinned commits below.
cmake_minimum_required(VERSION 3.24)
project(hamr CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(HAMR_LUAU_DIR "" CACHE PATH "A Luau checkout to build against (default: fetch the pinned commit)")
set(HAMR_CLUAU_DIR "" CACHE PATH "A cluau checkout to build against (default: fetch the pinned commit)")
set(HAMR_LUAU_COMMIT 0afee00064945abbeb6e6ef2b87633564c54e176)  # Luau 0.688
set(HAMR_CLUAU_COMMIT 8bae2b2534d624ffe780651419576cde5b1c1a49) # cluau 0.20.0

include(FetchContent)
if(NOT HAMR_LUAU_DIR)
    FetchContent_Declare(luau GIT_REPOSITORY https://github.com/luau-lang/luau.git
        GIT_TAG ${HAMR_LUAU_COMMIT} GIT_SHALLOW FALSE SOURCE_SUBDIR _none)
    FetchContent_MakeAvailable(luau)
    set(HAMR_LUAU_DIR ${luau_SOURCE_DIR})
endif()
if(NOT HAMR_CLUAU_DIR)
    FetchContent_Declare(cluau GIT_REPOSITORY https://github.com/Aosiee/cluau.git
        GIT_TAG ${HAMR_CLUAU_COMMIT} GIT_SHALLOW FALSE SOURCE_SUBDIR _none)
    FetchContent_MakeAvailable(cluau)
    set(HAMR_CLUAU_DIR ${cluau_SOURCE_DIR})
endif()

set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
add_subdirectory(${HAMR_LUAU_DIR} ${CMAKE_BINARY_DIR}/luau EXCLUDE_FROM_ALL)

file(GLOB HAMR_CLUAU_SOURCES CONFIGURE_DEPENDS ${HAMR_CLUAU_DIR}/src/translate/*.cpp)
add_library(hamr_cluau STATIC ${HAMR_CLUAU_SOURCES} ${HAMR_CLUAU_DIR}/src/check/Check.cpp)
target_include_directories(hamr_cluau PUBLIC ${HAMR_CLUAU_DIR}/include PRIVATE ${HAMR_CLUAU_DIR}/src/translate)
target_link_libraries(hamr_cluau PUBLIC Luau.Analysis)

# The standard library and the host's type declarations, embedded.
file(GLOB HAMR_STD CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/std/*.cluau)
set(HAMR_EMBEDDED ${HAMR_STD} ${CMAKE_CURRENT_SOURCE_DIR}/types/host.d.luau)
set(HAMR_EMBED_CPP ${CMAKE_BINARY_DIR}/hamr_embedded.cpp)
string(REPLACE ";" "|" HAMR_EMBEDDED_ARG "${HAMR_EMBEDDED}")
add_custom_command(OUTPUT ${HAMR_EMBED_CPP}
    COMMAND ${CMAKE_COMMAND} -DOUT=${HAMR_EMBED_CPP} "-DFILES=${HAMR_EMBEDDED_ARG}"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    VERBATIM
    DEPENDS ${HAMR_EMBEDDED} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
    COMMENT "Embedding hamr's standard library")

file(GLOB HAMR_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(hamr ${HAMR_SOURCES} ${HAMR_EMBED_CPP})
target_include_directories(hamr PRIVATE src)
target_link_libraries(hamr PRIVATE hamr_cluau Luau.VM Luau.Compiler Luau.Analysis)
target_compile_options(hamr PRIVATE -Wall -Wextra)
target_compile_definitions(hamr PRIVATE HAMR_VERSION="0.1.0")
