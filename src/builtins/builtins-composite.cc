
#include "src/builtins/builtins-utils-inl.h"
#include "src/builtins/builtins.h"
#include "src/common/globals.h"
#include "src/handles/maybe-handles.h"
#include "src/objects/casting.h"
#include "src/objects/contexts.h"
#include "src/objects/heap-object.h"
#include "src/objects/js-composite-inl.h"
#include "src/objects/js-composite.h"
#include "src/objects/js-function.h"

namespace v8 {
namespace internal {

BUILTIN(CompositeConstructor) {
  const char* const kMethodName = "Composite";
  HandleScope scope(isolate);

  // 1. If NewTarget is undefined, throw a TypeError exception.
  if (IsUndefined(*args.new_target(), isolate)) {
    THROW_NEW_ERROR_RETURN_FAILURE(
        isolate, NewTypeError(MessageTemplate::kConstructorNotFunction,
                              isolate->factory()->NewStringFromAsciiChecked(
                                  kMethodName)));
  }

  // 2. Let composite be ? OrdinaryCreateFromConstructor(NewTarget,
  //    "%Composite.prototype%", « [[DisposableState]],
  //    [[DisposeCapability]] »).
  DirectHandle<Map> map;
  DirectHandle<JSFunction> target = args.target();
  DirectHandle<JSReceiver> new_target = Cast<JSReceiver>(args.new_target());

  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
      isolate, map, JSFunction::GetDerivedMap(isolate, target, new_target));

  DirectHandle<JSComposite> composite =
      isolate->factory()->NewJSComposite(map);

  // 5. Return composite.
  return *composite;
}

}  // namespace internal
}  // namespace v8
