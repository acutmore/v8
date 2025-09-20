#include "src/builtins/builtins-utils-inl.h"
#include "src/builtins/builtins.h"
#include "src/common/globals.h"
#include "src/handles/maybe-handles.h"
#include "src/objects/casting.h"
#include "src/objects/contexts.h"
#include "src/objects/heap-object.h"
#include "src/objects/js-composite-inl.h"
#include "src/execution/isolate.h"
#include "src/handles/handles.h"
#include "src/objects/objects-inl.h"
#include "src/objects/js-composite.h"
#include "src/objects/smi.h"
#include "src/objects/string.h"
#include "src/objects/heap-number.h"
#include "src/objects/bigint.h"
#include "src/objects/js-objects.h"
#include "src/objects/js-function.h"
#include "src/objects/property-descriptor.h"
#include "src/objects/field-index-inl.h"
#include "src/objects/descriptor-array-inl.h"
#include "src/objects/map-inl.h"
#include "src/runtime/runtime.h"
#include "src/objects/hash-table.h"
#include "src/objects/fixed-array.h"

namespace v8 {
namespace internal {

bool AreCompositesStructurallyEqual(Isolate* isolate,
                                   DirectHandle<JSComposite> ac,
                                   DirectHandle<JSComposite> bc) {
  if (ac.is_identical_to(bc)) {
    return true;
  }

  Tagged<Map> a_map = ac->map();
  Tagged<Map> b_map = bc->map();

  if (a_map != b_map) {
    return false;
  }

  Tagged<DescriptorArray> descriptors = a_map->instance_descriptors();

  for (InternalIndex i : a_map->IterateOwnDescriptors()) {
    PropertyDetails details = descriptors->GetDetails(i);

    if (details.location() == PropertyLocation::kField &&
        details.kind() == PropertyKind::kData) {

      FieldIndex field_index = FieldIndex::ForDetails(a_map, details);
      Tagged<Object> av = ac->RawFastPropertyAt(field_index);
      Tagged<Object> bv = bc->RawFastPropertyAt(field_index);

      if (!Object::StrictEquals(av, bv)) {
        return false;
      }
    }
  }

  return true;
}

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

  // Use the default Composite map directly (assuming no subclassing)
  DirectHandle<NativeContext> native_context = isolate->native_context();
  DirectHandle<JSFunction> composite_constructor(native_context->js_composite_fun(), isolate);
  DirectHandle<Map> map(composite_constructor->initial_map(), isolate);

  DirectHandle<JSComposite> composite =
      isolate->factory()->NewJSComposite(map);

  base::Hasher hasher(0x9E3779B9);  // Golden ratio constant as seed

  // Fast path: install properties using map transitions + direct field writes.
  // Falls back to generic definition only if a dictionary map is encountered.
  DirectHandle<Map> current_map = map;
  int current_property_index = 0;  // Counts successfully added fast properties.
  bool use_fast_path = true;

  for (DirectHandle<Name> key : sorted_keys) {
    DirectHandle<Object> value;
    size_t int_index;
    if (key->AsIntegerIndex(&int_index)) {
      ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
          isolate, value,
          JSReceiver::GetElement(isolate, Cast<JSReceiver>(input),
                                 static_cast<uint32_t>(int_index)));
    } else {
      ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
          isolate, value,
          JSReceiver::GetProperty(isolate, Cast<JSReceiver>(input), key));
    }

    if (IsHeapNumber(*value) && Cast<HeapNumber>(*value)->value() == 0) {
      value = handle(Smi::FromInt(0), isolate);
    }

    hasher.AddHash(key->hash());

    if (IsJSComposite(*value)) {
      DirectHandle<JSComposite> nested_composite(Cast<JSComposite>(*value),
                                                isolate);
      Tagged<Smi> nested_hash = nested_composite->hashcode();
      hasher.AddHash(static_cast<uint32_t>(Smi::ToInt(nested_hash)));
    } else {
      Tagged<Smi> hash_smi = Object::GetOrCreateHash(*value, isolate);
      uint32_t value_hash = static_cast<uint32_t>(Smi::ToInt(hash_smi));
      hasher.AddHash(value_hash);
    }

    if (use_fast_path) {
      // Ensure we have an internalized name for transition (required by
      // Map::TransitionToDataProperty which DCHECKs IsUniqueName).
      DirectHandle<Name> internalized_key =
          isolate->factory()->InternalizeName(key);

      constexpr PropertyAttributes kAttrs =
          static_cast<PropertyAttributes>(READ_ONLY | DONT_DELETE);
      constexpr PropertyConstness kConstness = PropertyConstness::kConst;

      current_map = Map::TransitionToDataProperty(
          isolate, current_map, internalized_key, value, kAttrs, kConstness,
          StoreOrigin::kNamed);

      if (current_map->is_dictionary_map()) {
        use_fast_path = false;
      } else {
        JSObject::MigrateToMap(isolate, composite, current_map);
        PropertyDetails details = current_map->GetLastDescriptorDetails(isolate);
        composite->WriteToField(InternalIndex(current_property_index), details,
                                *value);
        current_property_index++;
        continue;
      }
    }

    MaybeDirectHandle<Object> slow_result =
        JSObject::SetOwnPropertyIgnoreAttributes(
            composite, key, value,
            static_cast<PropertyAttributes>(READ_ONLY | DONT_DELETE));
    if (slow_result.is_null()) {
      return ReadOnlyRoots(isolate).exception();
    }
  }

  Maybe<bool> result = JSReceiver::PreventExtensions(
      isolate, composite, kDontThrow);
  MAYBE_RETURN(result, ReadOnlyRoots(isolate).exception());

  uint32_t hashcode = static_cast<uint32_t>(hasher.hash());

  if (hashcode == 0) {
    hashcode = 1;
  }

  composite->set_hashcode(hashcode);

  // Check cache for existing Composite with same hash
  Handle<ObjectHashTable> cache(native_context->js_composite_cache(), isolate);
  DirectHandle<Smi> hash_key = handle(Smi::FromInt(static_cast<int32_t>(hashcode)), isolate);

  Tagged<Object> cached_entry = cache->Lookup(hash_key);
  if (IsTheHole(cached_entry, isolate)) {
    DirectHandle<WeakArrayList> single_list = isolate->factory()->NewWeakArrayList(1);
    MaybeObjectDirectHandle weak_composite = MaybeObjectDirectHandle::Weak(composite);
    single_list = WeakArrayList::Append(isolate, single_list, weak_composite);
    Handle<ObjectHashTable> new_cache = ObjectHashTable::Put(cache, hash_key, single_list);
    native_context->set_js_composite_cache(*new_cache);
  } else {
    DCHECK(IsWeakArrayList(cached_entry));
    DirectHandle<WeakArrayList> composite_list(Cast<WeakArrayList>(cached_entry), isolate);

    for (int i = 0; i < composite_list->length(); i++) {
      Tagged<MaybeObject> maybe_composite = composite_list->Get(i);
      if (maybe_composite.IsWeak()) {
        Tagged<JSComposite> existing_composite = Cast<JSComposite>(maybe_composite.GetHeapObjectAssumeWeak());
        DirectHandle<JSComposite> existing_handle(existing_composite, isolate);
        if (AreCompositesStructurallyEqual(isolate, composite, existing_handle)) {
          return *existing_handle;
        }
      }
    }

    MaybeObjectDirectHandle weak_composite = MaybeObjectDirectHandle::Weak(composite);
    DirectHandle<WeakArrayList> maybe_new_list = WeakArrayList::Append(isolate, composite_list, weak_composite);
    if (*maybe_new_list != *composite_list) {
      Handle<ObjectHashTable> new_cache = ObjectHashTable::Put(cache, hash_key, maybe_new_list);
      native_context->set_js_composite_cache(*new_cache);
    }
  }

  return *composite;
}

}  // namespace internal
}  // namespace v8
