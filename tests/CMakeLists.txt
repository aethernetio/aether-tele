# Copyright 2026 Aethernet Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required( VERSION 3.16 )

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake)

CPMAddPackage(
  NAME Unity
  GITHUB_REPOSITORY ThrowTheSwitch/Unity
  GIT_TAG master
)
# Keep Unity's library ABI consistent with telemetry's uint64_t assertions on
# both 32-bit and 64-bit hosts. Header-only defines leave x86 unity.c disabled.
target_compile_definitions(unity PUBLIC UNITY_SUPPORT_64)

add_executable(test-tele
  tele_test_common.cpp
  test-tele-main.cpp
  test-tele-core.cpp
  test-tele-config.cpp
  test-tele-traps.cpp
  test-tele-iostream.cpp
  test-tele-env.cpp
  test-tiered-int-wire.cpp)
target_link_libraries(test-tele PRIVATE aether::tele unity)

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  target_compile_options(test-tele PUBLIC /Zc:preprocessor)
endif()
target_compile_definitions(test-tele PUBLIC AE_PROJECT_VERSION="1.2.3")

add_test(NAME test-tele COMMAND $<TARGET_FILE:test-tele>)

find_package(Threads REQUIRED)
add_executable(test-raw-tele raw/flat.cpp raw/flat_healthy.cpp)
target_include_directories(test-raw-tele PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test-raw-tele PRIVATE aether::tele_raw unity Threads::Threads)
target_compile_definitions(test-raw-tele PRIVATE AE_TELE_RAW_DEBUG=1 AE_TELE_TEST_NATIVE_FILE=0)
add_test(NAME test-raw-tele COMMAND test-raw-tele)
add_executable(test-raw-reindexed raw/flat.cpp raw/flat_healthy.cpp)
target_include_directories(test-raw-reindexed PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test-raw-reindexed PRIVATE aether::tele_raw unity Threads::Threads)
target_compile_definitions(test-raw-reindexed PRIVATE AE_TELE_RAW_DEBUG=1 AE_TELE_TEST_NATIVE_FILE=0 RAW_TEST_TAG_INDEX=2000)
add_test(NAME test-raw-reindexed COMMAND test-raw-reindexed)
foreach(raw_target test-raw-tele test-raw-reindexed)
  add_custom_command(TARGET ${raw_target} POST_BUILD
    COMMAND $<TARGET_FILE:${raw_target}> --map $<TARGET_FILE_DIR:${raw_target}>/${raw_target}.map.json
    COMMENT "Exporting raw telemetry debug map")
endforeach()
add_executable(test-tele-routing raw/legacy_routing.cpp raw/legacy_order_ab.cpp raw/legacy_order_ba.cpp)
target_link_libraries(test-tele-routing PRIVATE aether::tele unity)
add_test(NAME test-tele-routing COMMAND test-tele-routing)
add_executable(test-statistics-concurrency raw/statistics_concurrency.cpp)
target_link_libraries(test-statistics-concurrency PRIVATE aether::tele unity Threads::Threads)
add_test(NAME test-statistics-concurrency COMMAND test-statistics-concurrency)
set_tests_properties(test-statistics-concurrency PROPERTIES TIMEOUT 30)
if(AE_TELE_BUILD_FILE_DRAIN)
  add_executable(test-file-drain raw/flat.cpp raw/flat_healthy.cpp)
  target_link_libraries(test-file-drain PRIVATE aether::tele_file unity)
  target_compile_definitions(test-file-drain PRIVATE AE_TELE_RAW_DEBUG=1 AE_TELE_TEST_NATIVE_FILE=1)
  add_test(NAME test-file-drain COMMAND test-file-drain)
  set_tests_properties(test-file-drain PROPERTIES TIMEOUT 60)
  add_executable(test-file-backend raw/file_drain.cpp)
  target_link_libraries(test-file-backend PRIVATE aether::tele_file unity)
  add_test(NAME test-file-backend COMMAND test-file-backend)
  set_tests_properties(test-file-backend PROPERTIES TIMEOUT 60)
endif()
set(AE_TELE_AETHER_SOURCE "" CACHE PATH "Optional Aether checkout for real telemetry API compatibility tests")
if(AE_TELE_AETHER_SOURCE)
  if(NOT EXISTS "${AE_TELE_AETHER_SOURCE}/aether/tele.h")
    message(FATAL_ERROR "AE_TELE_AETHER_SOURCE must contain aether/tele.h")
  endif()
  add_executable(test-aether-tele-api raw/aether_api_probe.cpp)
  target_link_libraries(test-aether-tele-api PRIVATE aether::tele unity)
  target_include_directories(test-aether-tele-api PRIVATE ${AE_TELE_AETHER_SOURCE})
  target_compile_definitions(test-aether-tele-api PRIVATE AE_DISTILLATION=1
    AE_TELE_ENABLED=1 AE_TELE_LOG_MODULES=AE_ALL AE_TELE_DEBUG_MODULES=AE_ALL
    AE_TELE_INFO_MODULES=AE_ALL AE_TELE_WARN_MODULES=AE_ALL AE_TELE_ERROR_MODULES=AE_ALL
    AE_TELE_METRICS_MODULES=AE_ALL AE_TELE_METRICS_DURATION=AE_ALL
    AE_TELE_LOG_TIME_POINT=AE_ALL AE_TELE_LOG_LEVEL_MODULE=AE_ALL AE_TELE_LOG_NAME=AE_ALL
    AE_TELE_LOG_LOCATION=AE_ALL AE_TELE_LOG_BLOB=AE_ALL)
  add_test(NAME test-aether-tele-api COMMAND test-aether-tele-api)
endif()

option(AE_TELE_RAW_TSAN "Instrument raw telemetry tests with ThreadSanitizer" OFF)
if(AE_TELE_RAW_TSAN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(test-raw-tele PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(test-raw-tele PRIVATE -fsanitize=thread)
endif()
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  if(TARGET test-file-drain)
    add_test(NAME test-file-drain-restart COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/raw/compact_restart.py
      --exe $<TARGET_FILE:test-file-drain>)
    set_tests_properties(test-file-drain-restart PROPERTIES TIMEOUT 60)
  endif()
  add_test(NAME test-raw-decoder-regressions COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/raw/compact_decoder_test.py)
  add_test(NAME test-raw-maps COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/raw/compact_maps.py
    --exe $<TARGET_FILE:test-raw-tele>
    --reindexed-exe $<TARGET_FILE:test-raw-reindexed>)
  add_test(NAME test-raw-compile-fail COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/raw/compile_fail.py
    --compiler ${CMAKE_CXX_COMPILER} --id ${CMAKE_CXX_COMPILER_ID}
    --generator ${CMAKE_GENERATOR} --platform "${CMAKE_GENERATOR_PLATFORM}"
    --include ${PROJECT_SOURCE_DIR}/src
    --include ${ae-numeric_SOURCE_DIR}
    --include ${gcem_SOURCE_DIR}/include
    --source ${CMAKE_CURRENT_SOURCE_DIR}/raw/wrong_space.cpp)
endif()

add_executable(test-raw-sink raw/sink_adapter.cpp)
target_link_libraries(test-raw-sink PRIVATE aether::tele unity Threads::Threads)
target_compile_definitions(test-raw-sink PRIVATE AE_TELE_RAW_DEBUG=1)
add_test(NAME test-raw-sink COMMAND test-raw-sink)

foreach(sink_fixture test-raw-multitu test-raw-no-exceptions)
  add_executable(${sink_fixture} raw/sink_multitu.cpp raw/sink_multitu_a.cpp
    raw/sink_multitu_b.cpp)
  target_link_libraries(${sink_fixture} PRIVATE aether::tele Threads::Threads)
  target_compile_definitions(${sink_fixture} PRIVATE AE_PROJECT_VERSION="1.2.3")
  add_test(NAME ${sink_fixture} COMMAND ${sink_fixture})
  set_tests_properties(${sink_fixture} PROPERTIES TIMEOUT 30)
endforeach()
target_compile_definitions(test-raw-no-exceptions PRIVATE AE_TEST_NO_EXCEPTIONS=1
  $<$<CXX_COMPILER_ID:MSVC>:_HAS_EXCEPTIONS=0>)
target_compile_options(test-raw-no-exceptions PRIVATE
  "$<$<CXX_COMPILER_ID:MSVC>:/GR-;/EHs-c->"
  "$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-fno-rtti;-fno-exceptions>")
