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

cmake_minimum_required(VERSION 3.16)
project(aether_tele_space_standalone LANGUAGES CXX)

if(NOT AE_TELE_SOURCE_DIR)
  set(AE_TELE_SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)
endif()

if(NOT TARGET aether::tele)
  set(AE_TELE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(AE_TELE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory(${AE_TELE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/aether-tele)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

option(AE_TELE_SPACE_REINDEX_BUILD
  "Relax overlapping-index static asserts for revision-B rebuilds" OFF)

function(space_target name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE aether::tele)
  target_include_directories(${name} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/src
  )
  target_compile_definitions(${name} PRIVATE AE_TELE_SPACE_HOST=1)
  if(AE_TELE_SPACE_REINDEX_BUILD)
    target_compile_definitions(${name} PRIVATE AE_TELE_SPACE_REINDEX_BUILD=1)
  endif()
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(${name} PRIVATE /Zc:preprocessor)
  endif()
endfunction()

space_target(space_standalone
  src/main.cpp
  src/unit_tests.cpp
  src/network_emit.cpp
  src/application_emit.cpp
)

space_target(space_stack_probe src/stack_probe.cpp src/network_emit.cpp)

add_executable(space_heap_probe
  src/heap_probe.cpp
  src/network_emit.cpp
  src/application_emit.cpp
)
target_link_libraries(space_heap_probe PRIVATE aether::tele)
target_include_directories(space_heap_probe PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/include
  ${CMAKE_CURRENT_LIST_DIR}/src
)
target_compile_definitions(space_heap_probe PRIVATE AE_TELE_SPACE_HOST=1 AE_SPACE_HEAP_TRACK=1)
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  target_compile_options(space_heap_probe PRIVATE /Zc:preprocessor)
endif()

add_executable(space_fp_baseline footprint/baseline.cpp)
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  target_compile_options(space_fp_baseline PRIVATE /O2)
else()
  target_compile_options(space_fp_baseline PRIVATE -O2)
endif()

foreach(fp compile_out one_space two_spaces)
  add_executable(space_fp_${fp} footprint/${fp}.cpp)
  target_link_libraries(space_fp_${fp} PRIVATE aether::tele)
  target_include_directories(space_fp_${fp} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/src
  )
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(space_fp_${fp} PRIVATE /Zc:preprocessor /O2)
  else()
    target_compile_options(space_fp_${fp} PRIVATE -O2)
  endif()
endforeach()
target_sources(space_fp_compile_out PRIVATE src/network_emit.cpp src/application_emit.cpp)
target_compile_definitions(space_fp_compile_out PRIVATE AE_TELE_SPACE_COMPILE_OUT=1)
target_sources(space_fp_one_space PRIVATE src/network_emit.cpp)
target_sources(space_fp_two_spaces PRIVATE src/network_emit.cpp src/application_emit.cpp)

set(SPACE_OUT ${CMAKE_CURRENT_BINARY_DIR}/space_out)
add_test(NAME space_unit COMMAND $<TARGET_FILE:space_standalone> --phase=unit)
add_test(NAME space_threads COMMAND $<TARGET_FILE:space_standalone> --phase=threads)
add_test(NAME space_stress COMMAND $<TARGET_FILE:space_standalone> --phase=stress)
add_test(NAME space_write COMMAND $<TARGET_FILE:space_standalone> --phase=write --dir=${SPACE_OUT})
set_tests_properties(space_write PROPERTIES FIXTURES_SETUP space_blobs)
add_test(NAME space_resume COMMAND $<TARGET_FILE:space_standalone> --phase=resume --dir=${SPACE_OUT})
set_tests_properties(space_resume PROPERTIES FIXTURES_REQUIRED space_blobs)
add_test(NAME space_decode_net
  COMMAND $<TARGET_FILE:space_standalone> --phase=decode --space network ${SPACE_OUT}/network.blob)
set_tests_properties(space_decode_net PROPERTIES FIXTURES_REQUIRED space_blobs)
add_test(NAME space_decode_app
  COMMAND $<TARGET_FILE:space_standalone> --phase=decode --space application ${SPACE_OUT}/application.blob)
set_tests_properties(space_decode_app PROPERTIES FIXTURES_REQUIRED space_blobs)
add_test(NAME space_checksum_mismatch
  COMMAND $<TARGET_FILE:space_standalone> --phase=decode --space application ${SPACE_OUT}/network.blob)
set_tests_properties(space_checksum_mismatch PROPERTIES FIXTURES_REQUIRED space_blobs WILL_FAIL TRUE)
add_test(NAME space_heap_probe COMMAND $<TARGET_FILE:space_heap_probe>)
add_test(NAME space_stack_probe COMMAND $<TARGET_FILE:space_stack_probe>)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME space_source_net
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/source_locations.py
      --space network --root ${CMAKE_CURRENT_LIST_DIR} --print-lines)
  add_test(NAME space_source_app
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/source_locations.py
      --space application --root ${CMAKE_CURRENT_LIST_DIR} --print-lines)
  add_test(NAME space_locations_net
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/source_locations.py
      --space network --root ${CMAKE_CURRENT_LIST_DIR}
      --exe $<TARGET_FILE:space_standalone>
      --blob ${SPACE_OUT}/network.blob)
  set_tests_properties(space_locations_net PROPERTIES FIXTURES_REQUIRED space_blobs)
  add_test(NAME space_locations_app
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/source_locations.py
      --space application --root ${CMAKE_CURRENT_LIST_DIR}
      --exe $<TARGET_FILE:space_standalone>
      --blob ${SPACE_OUT}/application.blob)
  set_tests_properties(space_locations_app PROPERTIES FIXTURES_REQUIRED space_blobs)
  add_test(NAME space_reindex_dry
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/reindex.py
      --space network --root ${CMAKE_CURRENT_LIST_DIR}
      --report ${SPACE_OUT}/network.json)
  set_tests_properties(space_reindex_dry PROPERTIES FIXTURES_REQUIRED space_blobs)

  set(_cf_includes
    ${AE_TELE_SOURCE_DIR}/src
    ${ae-numeric_SOURCE_DIR}
    ${aether-miscpp_SOURCE_DIR}/src
    ${gcem_SOURCE_DIR}/include
  )
  if(MSVC)
    set(_cf_cmd ${CMAKE_CXX_COMPILER} /nologo /std:c++20 /Zc:preprocessor /EHsc /c
      ${CMAKE_CURRENT_LIST_DIR}/compile_fail/duplicate_index.cpp
      /Fo${CMAKE_CURRENT_BINARY_DIR}/duplicate_index.obj)
    foreach(inc IN LISTS _cf_includes)
      list(APPEND _cf_cmd /I${inc})
    endforeach()
  else()
    set(_cf_cmd ${CMAKE_CXX_COMPILER} -std=c++20 -c
      ${CMAKE_CURRENT_LIST_DIR}/compile_fail/duplicate_index.cpp
      -o ${CMAKE_CURRENT_BINARY_DIR}/duplicate_index.o)
    foreach(inc IN LISTS _cf_includes)
      list(APPEND _cf_cmd -I${inc})
    endforeach()
  endif()
  add_test(NAME space_compile_fail_dup
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_LIST_DIR}/tools/expect_compile_fail.py ${_cf_cmd})
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  option(AE_TELE_SPACE_TSAN "Build space standalone with ThreadSanitizer" OFF)
  if(AE_TELE_SPACE_TSAN)
    target_compile_options(space_standalone PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(space_standalone PRIVATE -fsanitize=thread)
  endif()
  option(AE_TELE_SPACE_STACK_USAGE "Emit .su files from -fstack-usage" OFF)
  if(AE_TELE_SPACE_STACK_USAGE)
    target_compile_options(space_standalone PRIVATE -fstack-usage)
    target_compile_options(space_stack_probe PRIVATE -fstack-usage)
  endif()
endif()
