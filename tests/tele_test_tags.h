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

#ifndef AE_TELE_TEST_TAGS_H_
#define AE_TELE_TEST_TAGS_H_

#include "aether-tele/tags.h"

AE_TELE_MODULE(TestObj, 12, 1, 20);

AE_TAG(Zero, TestObj)
AE_TAG(One, TestObj)
AE_TAG(Two, TestObj)
AE_TAG(Three, TestObj)
AE_TAG(Four, TestObj)
AE_TAG(Test1, TestObj)
AE_TAG(Test2, TestObj)
AE_TAG(Test3, TestObj)

AE_TAG(Reserved8, TestObj)
AE_TAG(Reserved9, TestObj)
AE_TAG(Reserved10, TestObj)

AE_TAG(Test, TestObj)

#endif  // AE_TELE_TEST_TAGS_H_
