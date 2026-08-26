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

#ifndef DEMO_EMIT_H_
#define DEMO_EMIT_H_

#include <cstdint>

#include "demo/application_space.h"
#include "demo/network_space.h"

namespace demo {

void EmitPoll(network::Stream& stream);
void EmitPktRx(network::Stream& stream, std::uint32_t size);
void EmitPktTx(network::Stream& stream, std::uint32_t size);
void EmitRequest(network::Stream& stream, std::uint64_t duration_ticks);
void EmitReconnect(network::Stream& stream);
void EmitNetError(network::Stream& stream, std::uint32_t code);
void EmitByteCount(network::Stream& stream);
void EmitDisabledTap(network::Stream& stream);

void EmitFrame(application::Stream& stream);
void EmitJob(application::Stream& stream, std::uint32_t id);
void EmitWork(application::Stream& stream, std::uint64_t duration_ticks);
void EmitUserEvent(application::Stream& stream);
void EmitFatal(application::Stream& stream, std::uint32_t code);
void EmitJobsDone(application::Stream& stream);
void EmitDisabledAudit(application::Stream& stream);

}  // namespace demo

#endif  // DEMO_EMIT_H_
