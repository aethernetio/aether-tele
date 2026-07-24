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

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/CPM.cmake)

CPMAddPackage(
  NAME unity
  GITHUB_REPOSITORY ThrowTheSwitch/Unity
  GIT_TAG master
)

add_executable(test-tele
  tele_test_common.cpp
  test-tele-main.cpp
  test-tele-core.cpp
  test-tele-config.cpp
  test-tele-traps.cpp
  test-tele-iostream.cpp
  test-tele-env.cpp)
target_link_libraries(test-tele PRIVATE aether::tele unity)

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  target_compile_options(test-tele PUBLIC /Zc:preprocessor)
endif()
target_compile_definitions(test-tele PUBLIC AE_PROJECT_VERSION="1.2.3")

add_test(NAME test-tele COMMAND $<TARGET_FILE:test-tele>)
