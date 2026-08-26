/*
 * Copyright 2026 Aethernet Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef DEMO_WORKLOAD_H_
#define DEMO_WORKLOAD_H_

#include <cstdint>
#include <string>

int RunUnitTests();
int RunThreadTest();
int RunStressTest();
int RunHeapProbe();
int PhaseWrite(std::string const& dir);
int PhaseResume(std::string const& dir);
int PhaseDecode(std::string const& space, std::string const& blob_path,
                bool text);

#endif  // DEMO_WORKLOAD_H_
