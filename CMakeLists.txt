# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20)
project(decimal128_text_codec_c C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

if(MSVC)
    set(D128TEXT_WARN_FLAGS /W4)
else()
    set(D128TEXT_WARN_FLAGS -Wall -Wextra -Wconversion -Wsign-conversion)
endif()

# The shipped library. One translation unit, four exported symbols, no
# dependencies. Everything else in the source is static, which is this
# language's spelling of the reference's `private` -- so the archive carries
# the same encapsulation the Kotlin object does, and no internal name can
# collide with a consumer's.
#
# The internal-state assertions are OFF here, which is deliberate: a consumer
# compiles these sources into their own program and must not take an abort
# inside a library on data they do not control. The buffer-sizing checks are
# `demand` and stay live regardless.
add_library(decimal128_text_codec src/decimal128_text_codec.c)
target_include_directories(decimal128_text_codec PUBLIC include)
target_compile_options(decimal128_text_codec PRIVATE ${D128TEXT_WARN_FLAGS})

# The same source with the assertions armed. Tests link THIS, so a test run
# actually exercises the invariants -- linking the shipped archive would leave
# every verify in the library compiled out no matter what the test defines.
add_library(decimal128_text_codec_verify STATIC src/decimal128_text_codec.c)
target_include_directories(decimal128_text_codec_verify PUBLIC include)
target_compile_definitions(decimal128_text_codec_verify PRIVATE VERIFY_ENABLED=1)
target_compile_options(decimal128_text_codec_verify PRIVATE ${D128TEXT_WARN_FLAGS})

# The same source again, forced onto the synthesize-from-limbs path -- no
# __int128, which is what MSVC gets. Cheap to carry and the only way to know
# the fallback is exercised on a machine that has the native type.
add_library(decimal128_text_codec_portable STATIC src/decimal128_text_codec.c)
target_include_directories(decimal128_text_codec_portable PUBLIC include)
target_compile_definitions(decimal128_text_codec_portable PRIVATE VERIFY_ENABLED=1 D128TEXT_NO_INT128)
target_compile_options(decimal128_text_codec_portable PRIVATE ${D128TEXT_WARN_FLAGS})

# The conformance corpus is not carried here. decimal128-resources is mounted as
# a submodule at <repo>/resources -- the same mount every decimal128 port uses --
# and the tables are read from it directly. There is no copy, so there is nothing
# to drift.
set(D128TEXT_VECTOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources/text_bid_dpd")

if(NOT EXISTS "${D128TEXT_VECTOR_DIR}/text_bid128_dpd128_100.csv.txt")
    message(FATAL_ERROR
        "the decimal128-resources submodule is not checked out: no vectors at "
        "${D128TEXT_VECTOR_DIR}. Run: git submodule update --init --recursive")
endif()

foreach(t test_corpus test_bid_parse)
    add_executable(${t} tests/${t}.c)
    target_link_libraries(${t} PRIVATE decimal128_text_codec_verify)
    target_compile_definitions(${t} PRIVATE VECTOR_DIR="${D128TEXT_VECTOR_DIR}")
    target_compile_options(${t} PRIVATE ${D128TEXT_WARN_FLAGS})
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# The whole corpus again over the no-__int128 build of the library.
add_executable(test_corpus_portable tests/test_corpus.c)
target_link_libraries(test_corpus_portable PRIVATE decimal128_text_codec_portable)
target_compile_definitions(test_corpus_portable PRIVATE VECTOR_DIR="${D128TEXT_VECTOR_DIR}")
target_compile_options(test_corpus_portable PRIVATE ${D128TEXT_WARN_FLAGS})
add_test(NAME test_corpus_portable COMMAND test_corpus_portable)

# The declet primitives are static, so this one includes the translation unit
# rather than linking an archive -- the standard C way to test internals
# without widening the published surface.
add_executable(test_declet tests/test_declet.c)
target_include_directories(test_declet PRIVATE include src)
target_compile_definitions(test_declet PRIVATE VERIFY_ENABLED=1)
target_compile_options(test_declet PRIVATE ${D128TEXT_WARN_FLAGS})
add_test(NAME test_declet COMMAND test_declet)

# The full canonical table, 84,764 rows. It used to run only when a sibling
# checkout happened to be present; the submodule is always there, so it is now an
# unconditional test. It stays a separate registration rather than folding into
# test_corpus so the portable (no-__int128) build does not pay for it twice.
add_test(NAME test_corpus_canonical
         COMMAND test_corpus "${D128TEXT_VECTOR_DIR}/text_bid128_dpd128_canonical.csv.txt")
