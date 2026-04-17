// Copyright 2024 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/execution/isolate-inl.h"
#include "src/objects/js-objects-inl.h"
#include "src/runtime/runtime-utils.h"

namespace v8 {
namespace internal {

// %CompositeStats() — returns a plain object with four counters and resets them.
// Requires --allow-natives-syntax.
RUNTIME_FUNCTION(Runtime_CompositeStats) {
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());

  CompositeStats& s = isolate->composite_stats();

  DirectHandle<JSObject> result =
      isolate->factory()->NewJSObject(isolate->object_function());

  JSObject::AddProperty(
      isolate, result, "totalInsertions",
      isolate->factory()->NewNumber(static_cast<double>(s.total_insertions)),
      NONE);
  JSObject::AddProperty(
      isolate, result, "collisionInsertions",
      isolate->factory()->NewNumber(
          static_cast<double>(s.collision_insertions)),
      NONE);
  JSObject::AddProperty(
      isolate, result, "maxBucketSize",
      isolate->factory()->NewNumber(static_cast<double>(s.max_bucket_size)),
      NONE);
  JSObject::AddProperty(
      isolate, result, "totalEqualityChecks",
      isolate->factory()->NewNumber(
          static_cast<double>(s.total_equality_checks)),
      NONE);

  s = {};  // reset all counters
  return *result;
}

}  // namespace internal
}  // namespace v8
