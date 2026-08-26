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

#ifndef AETHER_TELE_SPACE_SINK_H_
#define AETHER_TELE_SPACE_SINK_H_

#include "aether-tele/levels.h"
#include "aether-tele/space/schema.h"
#include "aether-tele/space/storage.h"

namespace ae::tele::space {

template <typename Config>
class SpaceSink {
 public:
  using ConfigType = Config;
  using Storage = TeleStorage<Config>;
  using Stream = ::ae::tele::space::Stream<Config>;

  template <Level::underlined_t L, std::uint32_t ModuleId>
  static consteval SpaceTeleConfig GetTeleConfig() {
    return Config::template GetTeleConfig<L, ModuleId>();
  }

  static SpaceSink& Instance() {
    static SpaceSink sink;
    return sink;
  }

  void Attach(Storage* storage) { storage_ = storage; }
  Storage* storage() const { return storage_; }

  Stream MakeStream(StreamMarker marker) {
    return Stream(*storage_, marker);
  }

 private:
  Storage* storage_{};
};

}  // namespace ae::tele::space

#endif  // AETHER_TELE_SPACE_SINK_H_
