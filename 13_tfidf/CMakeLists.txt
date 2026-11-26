cmake_minimum_required(VERSION 3.24)
project(tfidf LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

add_library(core STATIC
    vendor/index/base.cpp
    vendor/index/index.cpp
    vendor/index/tokens.cpp
    vendor/tokenizer/tokenizer.cpp
    vendor/stemmer/stem.cpp
    vendor/boolean/query.cpp
    vendor/boolean/search.cpp
    vendor/positions/index.cpp
    vendor/positions/build.cpp
    vendor/positions/search.cpp
    vendor/positions/linguistics.cpp
    vendor/wordnet/lemma.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(core PRIVATE Threads::Threads)
target_compile_features(core PUBLIC cxx_std_20)
target_include_directories(core PUBLIC cpp vendor/positions vendor/tokenizer vendor/stemmer vendor/index vendor/boolean vendor/wordnet)
target_compile_options(core PRIVATE -Wall -Wextra -Wpedantic)

set(wordnet_source "${CMAKE_CURRENT_BINARY_DIR}/wordnet_data.cpp")
add_custom_command(OUTPUT "${wordnet_source}"
    COMMAND "${CMAKE_COMMAND}"
        "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/vendor/wordnet/lexicon.bin"
        "-DOUTPUT=${wordnet_source}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_wordnet.cmake"
    DEPENDS vendor/wordnet/lexicon.bin cmake/embed_wordnet.cmake
    VERBATIM)
target_sources(core PRIVATE "${wordnet_source}")
set_source_files_properties("${wordnet_source}" PROPERTIES COMPILE_OPTIONS -Wno-overlength-strings)

add_executable(positional cpp/main.cpp)
target_link_libraries(positional PRIVATE core)

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
