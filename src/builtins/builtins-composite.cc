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
#include "src/base/small-vector.h"

namespace v8 {
namespace internal {

// Fast path detection for Composite construction - similar to JSON stringifier
V8_INLINE bool CanFastConstructComposite(Tagged<JSObject> raw_object,
                                         Isolate* isolate) {
  DisallowGarbageCollection no_gc;
  if (IsCustomElementsReceiverMap(raw_object->map())) return false;
  if (!raw_object->HasFastProperties()) return false;
  auto roots = ReadOnlyRoots(isolate);
  auto elements = raw_object->elements();
  return elements == roots.empty_fixed_array() ||
         elements == roots.empty_slow_element_dictionary();
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

  DirectHandle<JSReceiver> input_receiver = Cast<JSReceiver>(input);

  // Use the default Composite map directly (assuming no subclassing)
  DirectHandle<NativeContext> native_context = isolate->native_context();
  DirectHandle<JSFunction> composite_constructor(native_context->js_composite_fun(), isolate);
  DirectHandle<Map> map(composite_constructor->initial_map(), isolate);

  DirectHandle<JSComposite> composite =
      isolate->factory()->NewJSComposite(map);

  base::Hasher hasher(0x9E3779B9);  // Golden ratio constant as seed

  // Check if we can use fast path
  bool can_use_fast_path = IsJSObject(*input_receiver) &&
                           CanFastConstructComposite(Cast<JSObject>(*input_receiver), isolate);

  if (can_use_fast_path) {
    // Fast path: iterate directly over descriptors
    DirectHandle<JSObject> input_object = Cast<JSObject>(input_receiver);
    PtrComprCageBase cage_base(isolate);
    DirectHandle<Map> input_map(input_object->map(cage_base), isolate);

    if (input_map->NumberOfOwnDescriptors() == 0) {
      // Empty object case
      composite->set_hashcode(1);  // Non-zero hash for empty composite
      return *composite;
    }

    // Use SmallVector for keys - most objects have few properties
    base::SmallVector<std::pair<DirectHandle<Name>, DirectHandle<Object>>, 16> properties;

    // Collect enumerable string properties
    for (InternalIndex i : input_map->IterateOwnDescriptors()) {
      Handle<Name> key_name;
      PropertyDetails details = PropertyDetails::Empty();
      {
        DisallowGarbageCollection no_gc;
        Tagged<DescriptorArray> descriptors =
            input_map->instance_descriptors(cage_base);
        Tagged<Name> name = descriptors->GetKey(i);
        if (!IsString(name, cage_base)) continue;  // Skip symbols
        key_name = handle(Cast<String>(name), isolate);
        details = descriptors->GetDetails(i);
      }
      if (details.IsDontEnum()) continue;  // Skip non-enumerable

      // Get property value
      DirectHandle<Object> value;
      if (details.location() == PropertyLocation::kField &&
          *input_map == input_object->map(cage_base)) {
        DCHECK_EQ(PropertyKind::kData, details.kind());
        FieldIndex field_index = FieldIndex::ForDetails(*input_map, details);
        // Use RawFastPropertyAt to avoid reboxing doubles
        value = handle(input_object->RawFastPropertyAt(field_index), isolate);
      } else {
        // Fallback for accessor properties or if map changed
        ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
            isolate, value, JSReceiver::GetProperty(isolate, input_receiver, key_name));
      }

      properties.emplace_back(key_name, value);
    }

    // Sort properties by key name
    std::sort(properties.begin(), properties.end(),
      [isolate](const auto& a, const auto& b) {
        return Name::CompareLessThan(isolate, a.first, b.first);
      });

    // Fast path: Pre-compute final map, then install all properties at once
    DirectHandle<Map> current_map = map;
    bool use_fast_path = true;

    // Pre-compute all internalized keys and build final map in one batch
    base::SmallVector<DirectHandle<Name>, 16> internalized_keys;
    internalized_keys.reserve(properties.size());

    for (const auto& prop : properties) {
      DirectHandle<Name> internalized_key = isolate->factory()->InternalizeName(prop.first);
      internalized_keys.push_back(internalized_key);
    }

    // Batch create all map transitions
    constexpr PropertyAttributes kAttrs =
        static_cast<PropertyAttributes>(READ_ONLY | DONT_DELETE);
    constexpr PropertyConstness kConstness = PropertyConstness::kConst;

    DirectHandle<Map> final_map = current_map;
    for (size_t i = 0; i < internalized_keys.size() && use_fast_path; ++i) {
      DirectHandle<Name> key = internalized_keys[i];
      DirectHandle<Object> value = properties[i].second;

      DirectHandle<Map> new_map = Map::TransitionToDataProperty(
          isolate, final_map, key, value, kAttrs, kConstness,
          StoreOrigin::kNamed);

      if (new_map->is_dictionary_map()) {
        use_fast_path = false;
        break;
      }
      final_map = new_map;
    }

    if (use_fast_path) {
      // Migrate to final map once, then write all properties directly
      JSObject::MigrateToMap(isolate, composite, final_map);
    }

    for (size_t i = 0; i < properties.size(); ++i) {
      DirectHandle<Name> key = properties[i].first;
      DirectHandle<Object> value = properties[i].second;

      // Normalize HeapNumber(0) to Smi(0)
      if (IsHeapNumber(*value) && Cast<HeapNumber>(*value)->value() == 0) {
        value = handle(Smi::FromInt(0), isolate);
      }

      hasher.AddHash(key->hash());

      if (IsJSComposite(*value)) {
        DirectHandle<JSComposite> nested_composite(Cast<JSComposite>(*value), isolate);
        Tagged<Smi> nested_hash = nested_composite->hashcode();
        hasher.AddHash(static_cast<uint32_t>(Smi::ToInt(nested_hash)));
      } else {
        Tagged<Smi> hash_smi = Object::GetOrCreateHash(*value, isolate);
        uint32_t value_hash = static_cast<uint32_t>(Smi::ToInt(hash_smi));
        hasher.AddHash(value_hash);
      }

      if (use_fast_path) {
        // Write property value directly - we already have the final map
        DisallowGarbageCollection no_gc;
        Tagged<DescriptorArray> descriptors = final_map->instance_descriptors();
        PropertyDetails details = descriptors->GetDetails(InternalIndex(i));
        composite->WriteToField(InternalIndex(i), details, *value);
      } else {
        // Generic slow path fallback
        MaybeDirectHandle<Object> slow_result =
            JSObject::SetOwnPropertyIgnoreAttributes(
                composite, key, value,
                static_cast<PropertyAttributes>(READ_ONLY | DONT_DELETE));
        if (slow_result.is_null()) {
          return ReadOnlyRoots(isolate).exception();
        }
      }
    }
  } else {
    // Slow path: use KeyAccumulator
    DirectHandle<FixedArray> keys;
    ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
        isolate, keys,
        KeyAccumulator::GetKeys(isolate, input_receiver,
                                KeyCollectionMode::kOwnOnly,
                                static_cast<PropertyFilter>(PropertyFilter::ONLY_ENUMERABLE |
                                                          PropertyFilter::SKIP_SYMBOLS),
                                GetKeysConversion::kConvertToString));

    base::SmallVector<DirectHandle<Name>, 16> sorted_keys;
    int length = keys->length();
    sorted_keys.reserve(length);
    for (int i = 0; i < length; ++i) {
      DirectHandle<Name> key(Cast<Name>(keys->get(i)), isolate);
      sorted_keys.push_back(key);
    }
    std::sort(sorted_keys.begin(), sorted_keys.end(),
      [isolate](const DirectHandle<Name>& a, const DirectHandle<Name>& b) {
        return Name::CompareLessThan(isolate, a, b);
      });

    // Fast path: install properties using map transitions + direct field writes.
    DirectHandle<Map> current_map = map;
    int current_property_index = 0;
    bool use_fast_path = true;

    for (DirectHandle<Name> key : sorted_keys) {
      DirectHandle<Object> value;
      size_t index;
      if (key->AsIntegerIndex(&index)) {
        ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
            isolate, value, JSReceiver::GetElement(isolate, input_receiver, static_cast<uint32_t>(index)));
      } else {
        ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
            isolate, value, JSReceiver::GetProperty(isolate, input_receiver, key));
      }

      if (IsHeapNumber(*value) && Cast<HeapNumber>(*value)->value() == 0) {
        value = handle(Smi::FromInt(0), isolate);
      }

      hasher.AddHash(key->hash());

      if (IsJSComposite(*value)) {
        DirectHandle<JSComposite> nested_composite(Cast<JSComposite>(*value), isolate);
        Tagged<Smi> nested_hash = nested_composite->hashcode();
        hasher.AddHash(static_cast<uint32_t>(Smi::ToInt(nested_hash)));
      } else {
        Tagged<Smi> hash_smi = Object::GetOrCreateHash(*value, isolate);
        uint32_t value_hash = static_cast<uint32_t>(Smi::ToInt(hash_smi));
        hasher.AddHash(value_hash);
      }

      if (use_fast_path) {
        // Ensure we have an internalized name for transition
        DirectHandle<Name> internalized_key =
            isolate->factory()->InternalizeName(key);

        constexpr PropertyAttributes kAttrs =
            static_cast<PropertyAttributes>(READ_ONLY | DONT_DELETE);
        constexpr PropertyConstness kConstness = PropertyConstness::kConst;

        current_map = Map::TransitionToDataProperty(
            isolate, current_map, internalized_key, value, kAttrs, kConstness,
            StoreOrigin::kNamed);

        if (current_map->is_dictionary_map()) {
          // Fallback: switch to generic definition path for this and remaining
          // properties.
          use_fast_path = false;
        } else {
          JSObject::MigrateToMap(isolate, composite, current_map);
          PropertyDetails details = current_map->GetLastDescriptorDetails(isolate);
          composite->WriteToField(InternalIndex(current_property_index), details,
                                  *value);
          current_property_index++;
          continue;  // Done with fast path for this property.
        }
      }

      // Generic slow path (first time dictionary encountered or after).
      MaybeDirectHandle<Object> slow_result =
          JSObject::SetOwnPropertyIgnoreAttributes(
              composite, key, value,
              static_cast<PropertyAttributes>(READ_ONLY | DONT_DELETE));
      if (slow_result.is_null()) {
        return ReadOnlyRoots(isolate).exception();
      }
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

// Helper function to compare two JSComposite objects for equality.
bool CompareComposites(Isolate* isolate,
                       DirectHandle<JSComposite> ac,
                       DirectHandle<JSComposite> bc) {
  if (ac.is_identical_to(bc)) {
    return true;
  }

  Tagged<Map> a_map = ac->map();
  Tagged<Map> b_map = bc->map();

  // If maps are different, the composites have different structure
  if (a_map != b_map) {
    return false;
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
        if (!CompareComposites(isolate,
              DirectHandle<JSComposite>(Cast<JSComposite>(av), isolate),
              DirectHandle<JSComposite>(Cast<JSComposite>(bv), isolate))) {
          return false;
        }
      } else {
        // TODO(AC): SameValueZero?
        if (!Object::StrictEquals(av, bv)) {
          return false;
        }
      }
    }
  }

  return true;
}

RUNTIME_FUNCTION(Runtime_CompositeEqualHelper) {
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());

  DirectHandle<JSComposite> ac = args.at<JSComposite>(0);
  DirectHandle<JSComposite> bc = args.at<JSComposite>(1);

  bool eq = CompareComposites(isolate, ac, bc);
  if (eq) return ReadOnlyRoots(isolate).true_value();
  return ReadOnlyRoots(isolate).false_value();
}

BUILTIN(CompositeEqualHelper) {
  HandleScope scope(isolate);
  DCHECK_EQ(3, args.length());

  // Get arguments directly from args
  DirectHandle<Context> context = args.at<Context>(0);
  DirectHandle<JSComposite> ac = args.at<JSComposite>(1);
  DirectHandle<JSComposite> bc = args.at<JSComposite>(2);

  bool eq = CompareComposites(isolate, ac, bc);
  if (eq) return ReadOnlyRoots(isolate).true_value();
  return ReadOnlyRoots(isolate).false_value();
}

}  // namespace internal
}  // namespace v8
