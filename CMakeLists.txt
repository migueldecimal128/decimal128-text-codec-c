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

set(D128TEXT_VECTOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors")

foreach(t test_corpus test_bid_tostring test_bid_parse test_dpd test_dectest)
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

# The divide kernel likewise: its whole domain needs direct calls, and the
# corpus reaches almost none of it (see README).
add_executable(test_divpow10 tests/test_divpow10.c)
target_include_directories(test_divpow10 PRIVATE include src)
target_compile_definitions(test_divpow10 PRIVATE VERIFY_ENABLED=1)
target_compile_options(test_divpow10 PRIVATE ${D128TEXT_WARN_FLAGS})
add_test(NAME test_divpow10 COMMAND test_divpow10)

# The full canonical table (84,764 rows) is not vendored -- it is 10 MB. When a
# sibling decimal128-resources checkout is present, run it too.
set(D128TEXT_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../decimal128-resources/string_bid_dpd")
set(D128TEXT_CANONICAL "${D128TEXT_RESOURCES}/string_bid128_dpd128_canonical.csv.txt")
if(EXISTS "${D128TEXT_CANONICAL}")
    add_test(NAME test_corpus_canonical COMMAND test_corpus "${D128TEXT_CANONICAL}")
endif()

# tests/vectors is this family's corpus of record, copied out of
# decimal128-resources. Copies drift silently, so when a sibling checkout is
# present, check them -- allowing the two documented exemptions and nothing
# else. CI runs the same script against a fresh clone, which is where it
# actually bites; this registration is for the developer who has the sibling.
if(NOT WIN32 AND EXISTS "${D128TEXT_RESOURCES}")
    add_test(NAME test_vendored_corpus_drift
             COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_vendored_corpus.sh"
                     "${D128TEXT_RESOURCES}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors")
endif()
