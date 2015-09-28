// Copyright 2015 Cloudera, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "kudu/util/malloc.h"

#ifdef linux
#include <malloc.h>
#else
#include <malloc/malloc.h>
#endif

namespace kudu {

int64_t kudu_malloc_usable_size(const void* obj) {
#ifdef linux
  return malloc_usable_size(const_cast<void*>(obj));
#else
  return malloc_size(obj);
#endif
}

} // namespace kudu
