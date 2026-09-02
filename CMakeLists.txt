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

project(aether-tele VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  set(IS_ROOT_PROJECT ON)
else()
  set(IS_ROOT_PROJECT OFF)
endif()

option(AE_TELE_BUILD_TESTS "Build aether-tele tests" ${IS_ROOT_PROJECT})
option(AE_TELE_INSTALL "Install aether-tele library" ${IS_ROOT_PROJECT})

include(GNUInstallDirs)
include(cmake/CPM.cmake)

CPMAddPackage(
  NAME ae-numeric
  GITHUB_REPOSITORY aethernetio/aethernet-numeric
  GIT_TAG 68fcbb3d7975adddf2be5fae4c8c22ef725da5e7
  OPTIONS
    "AE_NUMERIC_INSTALL ${AE_TELE_INSTALL}"
    "AE_BUILD_TESTS OFF"
)
CPMAddPackage(
  NAME aether-miscpp
  GITHUB_REPOSITORY aethernetio/aether-miscpp
  GIT_TAG main
  OPTIONS
    "AE_INSTALL ${AE_TELE_INSTALL}"
    "AE_BUILD_TESTS OFF"
)

add_library(aether-tele STATIC)
add_library(aether::tele ALIAS aether-tele)
set_target_properties(aether-tele PROPERTIES EXPORT_NAME tele)

target_sources(aether-tele PRIVATE
  src/aether-tele/traps/io_stream_traps.cpp
  src/aether-tele/traps/statistics_trap.cpp
)

target_include_directories(aether-tele PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/src>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(aether-tele PUBLIC cxx_std_20)
target_compile_options(aether-tele PUBLIC
  $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)
target_link_libraries(aether-tele PUBLIC aether-miscpp ae-numeric)

if(AE_TELE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(AE_TELE_INSTALL)
  install(TARGETS aether-tele EXPORT aether-teleTargets)
  install(DIRECTORY src/aether-tele DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.h")
  install(EXPORT aether-teleTargets FILE aether-teleTargets.cmake NAMESPACE aether:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aether-tele)
  include(CMakePackageConfigHelpers)
  configure_package_config_file(cmake/aether-teleConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/aether-teleConfig.cmake INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aether-tele)
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/aether-teleConfigVersion.cmake VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/aether-teleConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/aether-teleConfigVersion.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/aether-tele)
endif()
