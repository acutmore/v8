#ifndef V8_OBJECTS_COMPOSITE_H_
#define V8_OBJECTS_COMPOSITE_H_

#include "src/objects/js-objects.h"
#include "src/objects/heap-object.h"
#include "src/objects/tagged.h"

// Has to be the last include (doesn't have include guards):
#include "src/objects/object-macros.h"

namespace v8 {
namespace internal {

#include "torque-generated/src/objects/js-composite-tq.inc"

class JSComposite : public TorqueGeneratedJSComposite<JSComposite, JSObject> {
 public:
  inline uint32_t hashcode() const;
  inline void set_hashcode(uint32_t value);
  DECL_PRINTER(JSComposite)
  EXPORT_DECL_VERIFIER(JSComposite)

  class BodyDescriptor;

  TQ_OBJECT_CONSTRUCTORS(JSComposite)
};


}  // namespace internal
}  // namespace v8

#include "src/objects/object-macros-undef.h"

#endif  // V8_OBJECTS_COMPOSITE_H_
