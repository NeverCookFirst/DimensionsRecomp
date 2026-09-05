# xexdump - offline dumper for the decrypted guest image.
#
# The recompiled game cannot be launched from a sandboxed session, so anything
# that needs the *decrypted* XEX has to happen headlessly. The codegen already
# proves this works without a GPU or a window: Runtime with tool_mode = true
# loads and decrypts the image, and BinaryView hands back the sections.

cmake_minimum_required(VERSION 3.25)
project(xexdump LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(rexglue REQUIRED)

add_executable(xexdump src/main.cpp)
target_link_libraries(xexdump PRIVATE rex::runtime)
