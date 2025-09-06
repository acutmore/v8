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
#include "src/objects/field-index-inl.h"
#include "src/objects/descriptor-array-inl.h"
#include "src/objects/map-inl.h"
#include "src/objects/objects-inl.h"
#include "src/runtime/runtime.h"
#include "src/base/hashing.h"

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

  base::Hasher hasher(0x9E3779B9);  // Golden ratio constant as seed

  for (DirectHandle<Name> key : sorted_keys) {
    DirectHandle<Object> value;
    size_t index;
    if (key->AsIntegerIndex(&index)) {
      ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
          isolate, value, JSReceiver::GetElement(isolate, Cast<JSReceiver>(input), static_cast<uint32_t>(index)));
    } else {
      ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
          isolate, value, JSReceiver::GetProperty(isolate, Cast<JSReceiver>(input), key));
    }

    if (IsHeapNumber(*value) && Cast<HeapNumber>(*value)->value() == 0) {
      value = handle(Smi::FromInt(0), isolate);
    }

    hasher.AddHash(key->hash());

    if (IsJSComposite(*value)) {
      DirectHandle<JSComposite> nested_composite(Cast<JSComposite>(*value), isolate);
      hasher.AddHash(nested_composite->hashcode());
    } else {
      hasher.AddHash(Smi::ToInt(Object::GetHash(*value)));
    }

    if (key->AsIntegerIndex(&index)) {
      MaybeDirectHandle<Object> set_result = JSObject::SetOwnElementIgnoreAttributes(
          composite, static_cast<uint32_t>(index), value,
          static_cast<PropertyAttributes>(READ_ONLY | DONT_DELETE));
      if (set_result.is_null()) {
        return ReadOnlyRoots(isolate).exception();
      }
    } else {
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
  }

  Maybe<bool> result = JSReceiver::PreventExtensions(
      isolate, composite, kDontThrow);
  MAYBE_RETURN(result, ReadOnlyRoots(isolate).exception());

  uint32_t hashcode = static_cast<uint32_t>(hasher.hash());

  // Ensure composites have a non-zero hash
  // because the 'zero' hash has a special semantics within Map+Set logic
  if (hashcode == 0) {
    hashcode = 1;
  }

  composite->set_hashcode(hashcode);

  return *composite;
}

namespace {

// Helper function to compare two JSComposite objects for equality
Tagged<Object> CompareComposites(Isolate* isolate,
                                 DirectHandle<JSComposite> ac,
                                 DirectHandle<JSComposite> bc) {
  if (ac.is_identical_to(bc)) {
    return ReadOnlyRoots(isolate).true_value();
  }

  Tagged<Map> a_map = ac->map();
  Tagged<Map> b_map = bc->map();

  // If maps are different, the composites have different structure
  if (a_map != b_map) {
    return ReadOnlyRoots(isolate).false_value();
  }

  Tagged<DescriptorArray> descriptors = a_map->instance_descriptors();

  // Iterate through all own descriptors and compare property values
  for (InternalIndex i : a_map->IterateOwnDescriptors()) {
    PropertyDetails details = descriptors->GetDetails(i);

    // Composites should only have data properties
    if (details.location() == PropertyLocation::kField &&
        details.kind() == PropertyKind::kData) {

      FieldIndex field_index = FieldIndex::ForDetails(a_map, details);
      Tagged<Object> av = ac->RawFastPropertyAt(field_index);
      Tagged<Object> bv = bc->RawFastPropertyAt(field_index);

      if (IsJSComposite(av) && IsJSComposite(bv)) {
        if (CompareComposites(isolate,
              DirectHandle<JSComposite>(Cast<JSComposite>(av), isolate),
              DirectHandle<JSComposite>(Cast<JSComposite>(bv), isolate)) != ReadOnlyRoots(isolate).true_value()) {
          return ReadOnlyRoots(isolate).false_value();
        }
      } else {
        if (!Object::StrictEquals(av, bv)) {
          return ReadOnlyRoots(isolate).false_value();
        }
      }
    }
  }

  return ReadOnlyRoots(isolate).true_value();
}

}  // namespace

RUNTIME_FUNCTION(Runtime_CompositeEqualHelper) {
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());

  DirectHandle<JSComposite> ac = args.at<JSComposite>(0);
  DirectHandle<JSComposite> bc = args.at<JSComposite>(1);

  return CompareComposites(isolate, ac, bc);
}

BUILTIN(CompositeEqualHelper) {
  HandleScope scope(isolate);
  DCHECK_EQ(3, args.length());

  // Get arguments directly from args
  DirectHandle<Context> context = args.at<Context>(0);
  DirectHandle<JSComposite> ac = args.at<JSComposite>(1);
  DirectHandle<JSComposite> bc = args.at<JSComposite>(2);

  return CompareComposites(isolate, ac, bc);
}

}  // namespace internal
}  // namespace v8
