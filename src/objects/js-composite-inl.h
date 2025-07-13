#ifndef V8_OBJECTS_JS_COMPOSITE_INL_H_
#define V8_OBJECTS_JS_COMPOSITE_INL_H_

#include "src/objects/js-composite.h"
// Include the non-inl header before the rest of the headers.

#include "src/objects/heap-object-inl.h"
#include "src/objects/objects-inl.h"

// Has to be the last include (doesn't have include guards):
#include "src/objects/object-macros.h"

namespace v8 {
namespace internal {

#include "torque-generated/src/objects/js-composite-tq-inl.inc"

TQ_OBJECT_CONSTRUCTORS_IMPL(JSComposite)

inline uint32_t JSComposite::hashcode() const {
  return RELAXED_READ_INT32_FIELD(*this, kHashcodeOffset);
}
inline void JSComposite::set_hashcode(uint32_t value) {
  RELAXED_WRITE_INT32_FIELD(*this, kHashcodeOffset, value);
}


}  // namespace internal
}  // namespace v8

#include "src/objects/object-macros-undef.h"

#endif  // V8_OBJECTS_JS_COMPOSITE_INL_H_
