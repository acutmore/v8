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
#include "src/objects/transitions-inl.h"
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
  DirectHandle<Map> initial_map(composite_constructor->initial_map(), isolate);

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
      DirectHandle<JSComposite> composite =
          isolate->factory()->NewJSComposite(initial_map);
      composite->set_hashcode(1);  // Non-zero hash for empty composite
      return *composite;
    }

    // Collect enumerable string properties in sorted key order.
    // Cache the sort order per input map to avoid re-sorting on repeated calls.
    DirectHandle<FixedArray> sort_order;

    bool sort_cache_hit = false;
    if (v8_flags.composite_sort_cache) {
      Tagged<Object> cached_input_map_obj =
          native_context->js_composite_cached_input_map();
      if (!IsUndefined(cached_input_map_obj) &&
          cached_input_map_obj == *input_map) {
        sort_order = DirectHandle<FixedArray>(
            Cast<FixedArray>(native_context->js_composite_cached_sort_order()),
            isolate);
        sort_cache_hit = true;
      }
    }

    if (!sort_cache_hit) {
      struct KeyWithIndex {
        Handle<Name> key;
        int descriptor_index;
      };
      base::SmallVector<KeyWithIndex, 16> keys_to_sort;

      for (InternalIndex i : input_map->IterateOwnDescriptors()) {
        DisallowGarbageCollection no_gc;
        Tagged<DescriptorArray> descriptors =
            input_map->instance_descriptors(cage_base);
        Tagged<Name> name = descriptors->GetKey(i);
        if (!IsString(name, cage_base)) continue;  // Skip symbols
        PropertyDetails details = descriptors->GetDetails(i);
        if (details.IsDontEnum()) continue;  // Skip non-enumerable
        keys_to_sort.push_back({handle(name, isolate), i.as_int()});
      }

      std::sort(keys_to_sort.begin(), keys_to_sort.end(),
                [isolate](const KeyWithIndex& a, const KeyWithIndex& b) {
                  return Name::CompareLessThan(isolate, a.key, b.key);
                });

      sort_order = isolate->factory()->NewFixedArray(
          static_cast<int>(keys_to_sort.size()), AllocationType::kOld);
      for (size_t i = 0; i < keys_to_sort.size(); ++i) {
        sort_order->set(static_cast<int>(i),
                        Smi::FromInt(keys_to_sort[i].descriptor_index));
      }
      if (v8_flags.composite_sort_cache) {
        native_context->set_js_composite_cached_input_map(*input_map);
        native_context->set_js_composite_cached_sort_order(*sort_order);
      }
    }

    constexpr PropertyAttributes kAttrs =
        static_cast<PropertyAttributes>(READ_ONLY | DONT_DELETE);
    constexpr PropertyConstness kConstness = PropertyConstness::kConst;

    // Check cached map — compare descriptor count and key names
    Tagged<Map> cached = native_context->js_composite_cached_map();
    DirectHandle<Map> cached_map(cached, isolate);

    // Check if cached map is deprecated and update if needed
    if (cached_map->is_deprecated()) {
      cached_map = Map::Update(isolate, cached_map);
      native_context->set_js_composite_cached_map(*cached_map);
    }

    bool map_cache_hit = false;
    int num_props = sort_order->length();

    // Check for perfect match: same number of properties and all keys match
    if (cached_map->NumberOfOwnDescriptors() > 0 &&
        cached_map->NumberOfOwnDescriptors() == num_props) {
      bool perfect_match = true;
      Tagged<DescriptorArray> cached_descriptors = cached_map->instance_descriptors();
      Tagged<DescriptorArray> input_descriptors =
          input_map->instance_descriptors(cage_base);

      for (int i = 0; i < num_props; ++i) {
        InternalIndex desc_idx(Smi::ToInt(sort_order->get(i)));
        Tagged<Name> input_key = input_descriptors->GetKey(desc_idx);
        Tagged<Name> cached_key = cached_descriptors->GetKey(InternalIndex(i));
        if (!cached_key->Equals(input_key)) {
          perfect_match = false;
          break;
        }
        PropertyDetails details = cached_descriptors->GetDetails(InternalIndex(i));
        if (details.constness() != kConstness || details.attributes() != kAttrs) {
          perfect_match = false;
          break;
        }
      }

      if (perfect_match) {
        map_cache_hit = true;
      }
    }

    if (map_cache_hit) {
      // Fused fast path: allocate with cached map, then single-pass
      // read from input → hash → write to composite.
      DirectHandle<JSComposite> composite =
          isolate->factory()->NewJSComposite(cached_map);
      JSObject::AllocateStorageForMap(isolate, composite, cached_map);

      {
        DisallowGarbageCollection no_gc;
        Tagged<DescriptorArray> input_descriptors =
            input_map->instance_descriptors(cage_base);
        Tagged<DescriptorArray> out_descriptors =
            cached_map->instance_descriptors();

        for (int i = 0; i < num_props; ++i) {
          InternalIndex desc_idx(Smi::ToInt(sort_order->get(i)));
          PropertyDetails in_details = input_descriptors->GetDetails(desc_idx);

          // Get property value from input
          Tagged<Object> value;
          if (in_details.location() == PropertyLocation::kField &&
              *input_map == input_object->map(cage_base)) {
            DCHECK_EQ(PropertyKind::kData, in_details.kind());
            FieldIndex field_index = FieldIndex::ForDetails(*input_map, in_details);
            value = input_object->RawFastPropertyAt(field_index);
          } else {
            // Fallback for accessor properties or if map changed
            DirectHandle<Object> value_handle;
            Handle<Name> key_name(
                input_descriptors->GetKey(desc_idx), isolate);
            ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
                isolate, value_handle,
                JSReceiver::GetProperty(isolate, input_receiver, key_name));
            value = *value_handle;
          }

          // Normalize HeapNumber(0) to Smi(0)
          if (IsHeapNumber(value) && Cast<HeapNumber>(value)->value() == 0) {
            value = Smi::FromInt(0);
          }

          // Hash the key and value
          Tagged<Name> key = input_descriptors->GetKey(desc_idx);
          hasher.AddHash(key->hash());

          if (IsJSComposite(value)) {
            Tagged<Smi> nested_hash = Cast<JSComposite>(value)->hashcode();
            hasher.AddHash(static_cast<uint32_t>(Smi::ToInt(nested_hash)));
          } else {
            Tagged<Smi> hash_smi = Object::GetOrCreateHash(value, isolate);
            uint32_t value_hash = static_cast<uint32_t>(Smi::ToInt(hash_smi));
            hasher.AddHash(value_hash);
          }

          // Write property value directly
          PropertyDetails out_details = out_descriptors->GetDetails(InternalIndex(i));
          composite->WriteToField(InternalIndex(i), out_details, value);
        }
      }

      // Skip PreventExtensions if the map is already non-extensible
      if (cached_map->is_extensible()) {
        Maybe<bool> pe_result = JSReceiver::PreventExtensions(
            isolate, composite, kDontThrow);
        MAYBE_RETURN(pe_result, ReadOnlyRoots(isolate).exception());
      }

      uint32_t hashcode = static_cast<uint32_t>(hasher.hash());
      // Ensure composites have a non-zero hash
      // because the 'zero' hash has a special semantics within Map+Set logic
      if (hashcode == 0) hashcode = 1;
      composite->set_hashcode(hashcode);

      return *composite;
    }

    // Cache miss path: collect properties, resolve map, allocate, write
    base::SmallVector<std::pair<DirectHandle<Name>, DirectHandle<Object>>, 16> properties;

    for (int i = 0; i < num_props; i++) {
      InternalIndex desc_idx(Smi::ToInt(sort_order->get(i)));
      Handle<Name> key_name;
      PropertyDetails details = PropertyDetails::Empty();
      FieldIndex field_index;

      {
        DisallowGarbageCollection no_gc;
        Tagged<DescriptorArray> descriptors =
            input_map->instance_descriptors(cage_base);
        Tagged<Name> name = descriptors->GetKey(desc_idx);
        details = descriptors->GetDetails(desc_idx);
        field_index = FieldIndex::ForDetails(*input_map, details);
        key_name = handle(Cast<String>(name), isolate);
      }

      // Get property value
      DirectHandle<Object> value;
      if (details.location() == PropertyLocation::kField &&
          *input_map == input_object->map(cage_base)) {
        DCHECK_EQ(PropertyKind::kData, details.kind());
        value = handle(input_object->RawFastPropertyAt(field_index), isolate);
      } else {
        // Fallback for accessor properties or if map changed
        ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
            isolate, value, JSReceiver::GetProperty(isolate, input_receiver, key_name));
      }

      properties.emplace_back(key_name, value);
    }

    // Pre-compute all internalized keys for transition search
    base::SmallVector<DirectHandle<Name>, 16> internalized_keys;
    internalized_keys.reserve(properties.size());
    for (const auto& prop : properties) {
      DirectHandle<Name> internalized_key = isolate->factory()->InternalizeName(prop.first);
      internalized_keys.push_back(internalized_key);
    }

    // Search existing transitions
    DirectHandle<Map> final_map = initial_map;
    size_t matched_properties = 0;
    bool use_fast_path = true;

    for (size_t i = 0; i < internalized_keys.size(); ++i) {
      MaybeHandle<Map> maybe_next = TransitionsAccessor::SearchTransition(
          isolate, final_map, *internalized_keys[i], PropertyKind::kData, kAttrs);

      if (maybe_next.is_null()) break;

      DirectHandle<Map> next_map = maybe_next.ToHandleChecked();
      InternalIndex descriptor = next_map->LastAdded();
      PropertyDetails details = next_map->instance_descriptors(isolate)->GetDetails(descriptor);
      if (details.constness() != kConstness) break;

      final_map = next_map;
      matched_properties++;
    }

    // Create remaining transitions if needed
    for (size_t i = matched_properties; i < internalized_keys.size() && use_fast_path; ++i) {
      DirectHandle<Map> new_map = Map::TransitionToDataProperty(
          isolate, final_map, internalized_keys[i], properties[i].second,
          kAttrs, kConstness, StoreOrigin::kNamed);

      if (new_map->is_dictionary_map()) {
        use_fast_path = false;
        break;
      }
      final_map = new_map;
    }

    if (use_fast_path) {
      // Allocate directly with the final map
      DirectHandle<JSComposite> composite =
          isolate->factory()->NewJSComposite(final_map);
      JSObject::AllocateStorageForMap(isolate, composite, final_map);

      {
        DisallowGarbageCollection no_gc;
        Tagged<DescriptorArray> descriptors = final_map->instance_descriptors();

        for (size_t i = 0; i < properties.size(); ++i) {
          Tagged<Name> key = *properties[i].first;
          Tagged<Object> value = *properties[i].second;

          // Normalize HeapNumber(0) to Smi(0)
          if (IsHeapNumber(value) && Cast<HeapNumber>(value)->value() == 0) {
            value = Smi::FromInt(0);
          }

          hasher.AddHash(key->hash());

          if (IsJSComposite(value)) {
            Tagged<Smi> nested_hash = Cast<JSComposite>(value)->hashcode();
            hasher.AddHash(static_cast<uint32_t>(Smi::ToInt(nested_hash)));
          } else {
            Tagged<Smi> hash_smi = Object::GetOrCreateHash(value, isolate);
            uint32_t value_hash = static_cast<uint32_t>(Smi::ToInt(hash_smi));
            hasher.AddHash(value_hash);
          }

          PropertyDetails details = descriptors->GetDetails(InternalIndex(i));
          composite->WriteToField(InternalIndex(i), details, value);
        }
      }

      if (final_map->is_extensible()) {
        Maybe<bool> pe_result = JSReceiver::PreventExtensions(
            isolate, composite, kDontThrow);
        MAYBE_RETURN(pe_result, ReadOnlyRoots(isolate).exception());
      }

      uint32_t hashcode = static_cast<uint32_t>(hasher.hash());
      if (hashcode == 0) hashcode = 1;
      composite->set_hashcode(hashcode);

      // Cache the final map for future Composite constructions with same shape
      // Only cache maps that have properties and aren't in dictionary mode
      Tagged<Map> result_map = composite->map();
      if (result_map->NumberOfOwnDescriptors() > 0 &&
          !result_map->is_dictionary_map()) {
        native_context->set_js_composite_cached_map(result_map);
      }

      return *composite;
    } else {
      // Dictionary mode fallback
      DirectHandle<JSComposite> composite =
          isolate->factory()->NewJSComposite(initial_map);

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

        // Generic slow path fallback
        MaybeDirectHandle<Object> slow_result =
            JSObject::SetOwnPropertyIgnoreAttributes(
                composite, key, value,
                static_cast<PropertyAttributes>(READ_ONLY | DONT_DELETE));
        if (slow_result.is_null()) {
          return ReadOnlyRoots(isolate).exception();
        }
      }

      Maybe<bool> pe_result = JSReceiver::PreventExtensions(
          isolate, composite, kDontThrow);
      MAYBE_RETURN(pe_result, ReadOnlyRoots(isolate).exception());

      uint32_t hashcode = static_cast<uint32_t>(hasher.hash());
      if (hashcode == 0) hashcode = 1;
      composite->set_hashcode(hashcode);

      return *composite;
    }
  } else {
    // Slow path: use KeyAccumulator for non-fast objects (proxies, etc.)
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
    // Sort properties by key name for deterministic ordering
    std::sort(sorted_keys.begin(), sorted_keys.end(),
      [isolate](const DirectHandle<Name>& a, const DirectHandle<Name>& b) {
        return Name::CompareLessThan(isolate, a, b);
      });

    DirectHandle<JSComposite> composite =
        isolate->factory()->NewJSComposite(initial_map);

    // Fast path: install properties using map transitions + direct field writes.
    constexpr PropertyAttributes kAttrs =
        static_cast<PropertyAttributes>(READ_ONLY | DONT_DELETE);
    constexpr PropertyConstness kConstness = PropertyConstness::kConst;

    DirectHandle<Map> current_map = initial_map;
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

    Maybe<bool> result = JSReceiver::PreventExtensions(
        isolate, composite, kDontThrow);
    MAYBE_RETURN(result, ReadOnlyRoots(isolate).exception());

    uint32_t hashcode = static_cast<uint32_t>(hasher.hash());
    // Ensure composites have a non-zero hash
    // because the 'zero' hash has a special semantics within Map+Set logic
    if (hashcode == 0) hashcode = 1;
    composite->set_hashcode(hashcode);

    // Cache the final map for future Composite constructions with same shape
    // Only cache maps that have properties and aren't in dictionary mode
    Tagged<Map> final_composite_map = composite->map();
    if (final_composite_map->NumberOfOwnDescriptors() > 0 &&
        !final_composite_map->is_dictionary_map()) {
      native_context->set_js_composite_cached_map(final_composite_map);
    }

    return *composite;
  }
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

  if (ac->hashcode() != bc->hashcode()) {
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
