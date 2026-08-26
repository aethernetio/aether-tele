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

void EmitFrame(application::Stream& stream) {
  APP_TELE_INFO(stream, demo::application::kFrame);
}
void EmitJob(application::Stream& stream, std::uint32_t id) {
  APP_TELE_INFO(stream, demo::application::kJob, id);
}
void EmitWork(application::Stream& stream, std::uint64_t duration_ticks) {
  APP_TELE_INFO(stream, demo::application::kWork);
  application::Clock::advance(duration_ticks);
}
void EmitUserEvent(application::Stream& stream) {
  APP_TELE_INFO(stream, demo::application::kUserEvent);
}
void EmitFatal(application::Stream& stream, std::uint32_t code) {
  APP_TELE_ERROR(stream, demo::application::kFatal, code);
}
void EmitJobsDone(application::Stream& stream) {
  APP_TELE_INFO(stream, demo::application::kJobsDone);
}
void EmitDisabledAudit(application::Stream& stream) {
  APP_TELE_DEBUG(stream, demo::application::kDisabledAudit);
}

}  // namespace demo
