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

#include "emit.h"

namespace demo {

void EmitPoll(network::Stream& stream) {
  NET_TELE_INFO(stream, demo::network::kPoll);
}
void EmitPktRx(network::Stream& stream, std::uint32_t size) {
  NET_TELE_INFO(stream, demo::network::kPktRx, size);
}
void EmitPktTx(network::Stream& stream, std::uint32_t size) {
  NET_TELE_INFO(stream, demo::network::kPktTx, size);
}
void EmitRequest(network::Stream& stream, std::uint64_t duration_ticks) {
  NET_TELE_INFO(stream, demo::network::kRequest);
  network::Clock::advance(duration_ticks);
}
void EmitReconnect(network::Stream& stream) {
  NET_TELE_WARNING(stream, demo::network::kReconnect);
}
void EmitNetError(network::Stream& stream, std::uint32_t code) {
  NET_TELE_ERROR(stream, demo::network::kNetError, code);
}
void EmitByteCount(network::Stream& stream) {
  NET_TELE_INFO(stream, demo::network::kByteCount);
}
void EmitDisabledTap(network::Stream& stream) {
  NET_TELE_DEBUG(stream, demo::network::kDisabledTap);
}

}  // namespace demo
