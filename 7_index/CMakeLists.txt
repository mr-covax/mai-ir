cmake_minimum_required(VERSION 3.24)
project(index LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

add_library(core STATIC
    cpp/base.cpp
    cpp/index.cpp
    cpp/tokens.cpp
    vendor/tokenizer/tokenizer.cpp
    vendor/stemmer/stem.cpp
)
target_compile_features(core PUBLIC cxx_std_20)
target_include_directories(core PUBLIC cpp vendor/tokenizer vendor/stemmer)
target_compile_options(core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(ir cpp/main.cpp)
target_link_libraries(ir PRIVATE core)

include(CTest)
if(BUILD_TESTING)
    find_package(GTest 1.15.2 CONFIG QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
            URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(googletest)
    endif()
    add_executable(unit_tests tests/core.cpp)
    target_link_libraries(unit_tests PRIVATE core GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(unit_tests)
endif()
