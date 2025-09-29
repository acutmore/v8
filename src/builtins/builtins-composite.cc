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
#include "src/base/small-vector.h"

namespace v8 {
namespace internal {

// CompositeKey - lightweight stack object for cache lookups, similar to StringTableKey
class CompositeKey {
 public:
  struct Property {
    DirectHandle<Name> key;          // Original key
    DirectHandle<Object> value;

    Property(DirectHandle<Name> k, DirectHandle<Object> v)
      : key(k), value(v) {}
  };

  CompositeKey(uint32_t hash, base::SmallVector<Property, 16>&& props)
    : hashcode_(hash), properties_(std::move(props)) {}

  uint32_t hash() const { return hashcode_; }
  const base::SmallVector<Property, 16>& properties() const { return properties_; }

  // Compare this key with an existing composite
  bool IsMatch(Isolate* isolate, Tagged<JSComposite> composite) {
    Tagged<Map> composite_map = composite->map();
    Tagged<DescriptorArray> descriptors = composite_map->instance_descriptors();

    if (composite_map->NumberOfOwnDescriptors() != static_cast<int>(properties_.size())) {
      return false;
    }

    // Compare properties directly - no sorting needed since composite properties
    // are already stored in sorted order by key name
    size_t prop_index = 0;
    for (InternalIndex i : composite_map->IterateOwnDescriptors()) {
      PropertyDetails details = descriptors->GetDetails(i);
      if (details.location() != PropertyLocation::kField ||
          details.kind() != PropertyKind::kData) {
        return false;
      }

      Tagged<Name> composite_key = descriptors->GetKey(i);
      FieldIndex field_index = FieldIndex::ForDetails(composite_map, details);
      Tagged<Object> composite_value = composite->RawFastPropertyAt(field_index);

      const Property& our_prop = properties_[prop_index];

      if (!composite_key->Equals(*our_prop.key)) {
        return false;
      }

      if (!Object::StrictEquals(*our_prop.value, composite_value)) {
        return false;
      }

      prop_index++;
    }

    return prop_index == properties_.size();
  }

  // Create the actual composite if no match found
  DirectHandle<JSComposite> CreateComposite(Isolate* isolate) {
    DirectHandle<NativeContext> native_context = isolate->native_context();
    DirectHandle<JSFunction> composite_constructor(native_context->js_composite_fun(), isolate);
    DirectHandle<Map> map(composite_constructor->initial_map(), isolate);
    DirectHandle<JSComposite> composite = isolate->factory()->NewJSComposite(map);

    // Use fast path map transitions if possible
    DirectHandle<Map> final_map = map;
    bool use_fast_path = true;

    constexpr PropertyAttributes kAttrs =
      static_cast<PropertyAttributes>(READ_ONLY | DONT_DELETE);
    constexpr PropertyConstness kConstness = PropertyConstness::kConst;

    for (size_t i = 0; i < properties_.size() && use_fast_path; ++i) {
      const Property& prop = properties_[i];
      DirectHandle<Name> internalized_key = isolate->factory()->InternalizeName(prop.key);

      DirectHandle<Map> new_map = Map::TransitionToDataProperty(
          isolate, final_map, internalized_key, prop.value, kAttrs, kConstness,
          StoreOrigin::kNamed);

      if (new_map->is_dictionary_map()) {
        use_fast_path = false;
        break;
      }
      final_map = new_map;
    }

    if (use_fast_path) {
      JSObject::MigrateToMap(isolate, composite, final_map);
      for (size_t i = 0; i < properties_.size(); ++i) {
        DisallowGarbageCollection no_gc;
        Tagged<DescriptorArray> descriptors = final_map->instance_descriptors();
        PropertyDetails details = descriptors->GetDetails(InternalIndex(i));
        composite->WriteToField(InternalIndex(i), details, *properties_[i].value);
      }
    } else {
      // Fallback to slow path
      for (const Property& prop : properties_) {
        MaybeDirectHandle<Object> result = JSObject::SetOwnPropertyIgnoreAttributes(
            composite, prop.key, prop.value, kAttrs);
        if (result.is_null()) {
          return DirectHandle<JSComposite>();  // Exception
        }
      }
    }

    Maybe<bool> prevent_result = JSReceiver::PreventExtensions(isolate, composite, kDontThrow);
    if (prevent_result.IsNothing()) {
      return DirectHandle<JSComposite>();  // Exception
    }

    composite->set_hashcode(hashcode_);
    return composite;
  }

 private:
  uint32_t hashcode_;
  base::SmallVector<Property, 16> properties_;
};

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

// Cache lookup using CompositeKey - similar to StringTable::LookupKey
DirectHandle<JSComposite> LookupCompositeWithKey(Isolate* isolate, CompositeKey* key) {
  DirectHandle<NativeContext> native_context = isolate->native_context();
  Handle<ObjectHashTable> cache(native_context->js_composite_cache(), isolate);
  DirectHandle<Smi> hash_key = handle(Smi::FromInt(static_cast<int32_t>(key->hash())), isolate);

  Tagged<Object> cached_entry = cache->Lookup(hash_key);
  if (!IsTheHole(cached_entry, isolate)) {
    DCHECK(IsWeakArrayList(cached_entry));
    DirectHandle<WeakArrayList> composite_list(Cast<WeakArrayList>(cached_entry), isolate);

    for (int i = 0; i < composite_list->length(); i++) {
      Tagged<MaybeObject> maybe_composite = composite_list->Get(i);
      if (maybe_composite.IsWeak()) {
        Tagged<JSComposite> existing_composite = Cast<JSComposite>(maybe_composite.GetHeapObjectAssumeWeak());
        if (key->IsMatch(isolate, existing_composite)) {
          return handle(existing_composite, isolate);
        }
      }
    }
  }

  // Cache miss - create new composite
  DirectHandle<JSComposite> new_composite = key->CreateComposite(isolate);
  if (new_composite.is_null()) {
    return new_composite;  // Exception occurred
  }

  // Add to cache
  if (IsTheHole(cached_entry, isolate)) {
    DirectHandle<WeakArrayList> single_list = isolate->factory()->NewWeakArrayList(1);
    MaybeObjectDirectHandle weak_composite = MaybeObjectDirectHandle::Weak(new_composite);
    single_list = WeakArrayList::Append(isolate, single_list, weak_composite);
    Handle<ObjectHashTable> new_cache = ObjectHashTable::Put(cache, hash_key, single_list);
    native_context->set_js_composite_cache(*new_cache);
  } else {
    DirectHandle<WeakArrayList> composite_list(Cast<WeakArrayList>(cached_entry), isolate);
    MaybeObjectDirectHandle weak_composite = MaybeObjectDirectHandle::Weak(new_composite);
    DirectHandle<WeakArrayList> maybe_new_list = WeakArrayList::Append(isolate, composite_list, weak_composite);
    if (*maybe_new_list != *composite_list) {
      Handle<ObjectHashTable> new_cache = ObjectHashTable::Put(cache, hash_key, maybe_new_list);
      native_context->set_js_composite_cache(*new_cache);
    }
  }

  return new_composite;
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

  // Check if we can use fast path
  bool can_use_fast_path = IsJSObject(*input_receiver) &&
                           CanFastConstructComposite(Cast<JSObject>(*input_receiver), isolate);

  base::SmallVector<CompositeKey::Property, 16> properties;

  if (can_use_fast_path) {
    // Fast path: iterate directly over descriptors
    DirectHandle<JSObject> input_object = Cast<JSObject>(input_receiver);
    PtrComprCageBase cage_base(isolate);
    DirectHandle<Map> input_map(input_object->map(cage_base), isolate);

    if (input_map->NumberOfOwnDescriptors() == 0) {
      // Empty object case - create key directly
      CompositeKey empty_key(1, std::move(properties));
      return *LookupCompositeWithKey(isolate, &empty_key);
    }

    // Collect enumerable string properties
    for (InternalIndex i : input_map->IterateOwnDescriptors()) {
      Handle<Name> key_name;
      PropertyDetails details = PropertyDetails::Empty();
      FieldIndex field_index;

      {
        DisallowGarbageCollection no_gc;
        Tagged<DescriptorArray> descriptors = input_map->instance_descriptors(cage_base);
        Tagged<Name> name = descriptors->GetKey(i);
        if (!IsString(name, cage_base)) continue;  // Skip symbols
        key_name = handle(Cast<String>(name), isolate);
        details = descriptors->GetDetails(i);
        field_index = FieldIndex::ForDetails(*input_map, details);
      }
      if (details.IsDontEnum()) continue;  // Skip non-enumerable

      // Get property value
      DirectHandle<Object> value;
      if (details.location() == PropertyLocation::kField &&
          *input_map == input_object->map(cage_base)) {
        DCHECK_EQ(PropertyKind::kData, details.kind());
        // Use RawFastPropertyAt to avoid reboxing doubles
        value = handle(input_object->RawFastPropertyAt(field_index), isolate);
      } else {
        // Fallback for accessor properties or if map changed
        ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
            isolate, value, JSReceiver::GetProperty(isolate, input_receiver, key_name));
      }

      // Normalize HeapNumber(0) to Smi(0)
      if (IsHeapNumber(*value) && Cast<HeapNumber>(*value)->value() == 0) {
        value = handle(Smi::FromInt(0), isolate);
      }

      properties.emplace_back(key_name, value);
    }

    // Sort properties by key name
    std::sort(properties.begin(), properties.end(),
      [isolate](const CompositeKey::Property& a, const CompositeKey::Property& b) {
        return Name::CompareLessThan(isolate, a.key, b.key);
      });

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

      properties.emplace_back(key, value);
    }
  }

  // Compute hash from properties
  base::Hasher hasher(0x9E3779B9);  // Golden ratio constant as seed

  for (const CompositeKey::Property& prop : properties) {
    hasher.AddHash(prop.key->hash());

    if (IsJSComposite(*prop.value)) {
      DirectHandle<JSComposite> nested_composite(Cast<JSComposite>(*prop.value), isolate);
      Tagged<Smi> nested_hash = nested_composite->hashcode();
      hasher.AddHash(static_cast<uint32_t>(Smi::ToInt(nested_hash)));
    } else {
      Tagged<Smi> hash_smi = Object::GetOrCreateHash(*prop.value, isolate);
      uint32_t value_hash = static_cast<uint32_t>(Smi::ToInt(hash_smi));
      hasher.AddHash(value_hash);
    }
  }

  int32_t smi_hashcode = static_cast<uint32_t>(hasher.hash()) & 0x3FFFFFFF;
  if (smi_hashcode == 0) {
    smi_hashcode = 1;
  }

  // Create CompositeKey and lookup in cache
  CompositeKey key(static_cast<uint32_t>(smi_hashcode), std::move(properties));
  return *LookupCompositeWithKey(isolate, &key);
}

}  // namespace internal
}  // namespace v8
