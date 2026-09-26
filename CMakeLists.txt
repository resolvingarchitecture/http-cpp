cmake_minimum_required(VERSION 3.16)
project(http LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The envelope Send() reads/writes is ra::common::Envelope, so http-cpp
# depends on its ra-common-cpp sibling directly - mirroring http-java
# depending only on ra-common (see DESIGN.md).
set(RA_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../common/ra-common-cpp ${CMAKE_CURRENT_BINARY_DIR}/ra-common-cpp)

find_package(OpenSSL REQUIRED)

# Header-only library.
add_library(http INTERFACE)
target_include_directories(http INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(http INTERFACE ra_common OpenSSL::SSL OpenSSL::Crypto)
target_compile_features(http INTERFACE cxx_std_20)

option(HTTP_BUILD_TESTS "Build http-cpp tests" ON)
if(HTTP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
