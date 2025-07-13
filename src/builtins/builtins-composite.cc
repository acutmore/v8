
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
#include "src/objects/property-descriptor.h"

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

  DirectHandle<Object> input = args.atOrUndefined(isolate, 1);
  if (!IsJSReceiver(*input)) {
    THROW_NEW_ERROR_RETURN_FAILURE(
        isolate, NewTypeError(MessageTemplate::kCalledOnNonObject,
                              isolate->factory()->NewStringFromAsciiChecked(
                                  kMethodName)));
  }

  DirectHandle<FixedArray> keys;
  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
      isolate, keys,
      KeyAccumulator::GetKeys(isolate, Cast<JSReceiver>(input),
                              KeyCollectionMode::kOwnOnly,
                              static_cast<PropertyFilter>(PropertyFilter::ONLY_ENUMERABLE |
                                                        PropertyFilter::SKIP_SYMBOLS),
                              GetKeysConversion::kConvertToString));

  std::vector<DirectHandle<Name>> sorted_keys;
  int length = keys->length();
  sorted_keys.reserve(length);
  for (int i = 0; i < length; ++i) {
    DirectHandle<Name> key(Cast<Name>(keys->get(i)), isolate);
    sorted_keys.push_back(key);
  }
  std::sort(sorted_keys.begin(), sorted_keys.end(),
    [isolate](const DirectHandle<Name>& a, const DirectHandle<Name>& b) { return Name::CompareLessThan(isolate, a, b); });

  DirectHandle<Map> map;
  DirectHandle<JSFunction> target = args.target();
  DirectHandle<JSReceiver> new_target = Cast<JSReceiver>(args.new_target());

  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
      isolate, map, JSFunction::GetDerivedMap(isolate, target, new_target));

  DirectHandle<JSComposite> composite =
      isolate->factory()->NewJSComposite(map);

  for (DirectHandle<Name> key : sorted_keys) {
    DirectHandle<Object> value;
    ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
        isolate, value, JSReceiver::GetProperty(isolate, Cast<JSReceiver>(input), key));

    PropertyDescriptor desc;
    desc.set_value(Cast<JSAny>(value));
    desc.set_writable(false);
    desc.set_enumerable(true);
    desc.set_configurable(false);
    Maybe<bool> success = JSReceiver::DefineOwnProperty(
        isolate, composite, key, &desc,
        Just(kThrowOnError));
    MAYBE_RETURN(success, ReadOnlyRoots(isolate).exception());
    CHECK(success.FromJust());
  }

  Maybe<bool> result = JSReceiver::PreventExtensions(
      isolate, composite, kDontThrow);
  MAYBE_RETURN(result, ReadOnlyRoots(isolate).exception());

  // TODO hash
  composite->set_hashcode(0);

  return *composite;
}

}  // namespace internal
}  // namespace v8
