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
#include "src/heap/factory.h"

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

  DirectHandle<JSFunction> target = args.target();
  DirectHandle<JSReceiver> new_target = Cast<JSReceiver>(args.new_target());
  DirectHandle<NativeContext> native_context = isolate->native_context();
  DirectHandle<JSFunction> root_composite(native_context->js_composite_fun(),
                                          isolate);

  // If called as new Composite("name", ...), act as a factory: create and
  // return a specialized constructor with a pre-defined fast property shape.
  if (target.is_identical_to(root_composite)) {
    const int namec = std::max(0, args.length() - 1);
    if (namec == 0) {
      THROW_NEW_ERROR_RETURN_FAILURE(
          isolate, NewTypeError(MessageTemplate::kCalledOnNonObject,
                                isolate->factory()->NewStringFromAsciiChecked(
                                    "Composite requires at least one field name")));
    }

    std::vector<DirectHandle<Name>> original_names;
    original_names.reserve(namec);
    for (int i = 0; i < namec; ++i) {
      DirectHandle<Object> v = args.atOrUndefined(isolate, i + 1);
      if (!IsString(*v)) {
        THROW_NEW_ERROR_RETURN_FAILURE(
            isolate, NewTypeError(MessageTemplate::kCalledOnNonObject,
                                  isolate->factory()->NewStringFromAsciiChecked(
                                      "Composite field names must be strings")));
      }
      // Internalize as Name for transitions and hashing.
      original_names.push_back(
          isolate->factory()->InternalizeString(Cast<String>(v)));
    }

    Factory* factory = isolate->factory();

    // Build a canonical, sorted list of property names to define the shape.
    std::vector<DirectHandle<Name>> sorted_names = original_names;
    std::sort(sorted_names.begin(), sorted_names.end(), [isolate](
                  const DirectHandle<Name>& a, const DirectHandle<Name>& b) {
      return Name::CompareLessThan(isolate, a, b);
    });

    // Start from the root Composite initial map and transition to install
    // readonly, non-deletable data fields in sorted order to canonicalize
    // the structure regardless of declaration order.
    DirectHandle<Map> base_map(root_composite->initial_map(), isolate);
    DirectHandle<Map> final_map = base_map;
    constexpr PropertyAttributes kAttrs =
        static_cast<PropertyAttributes>(READ_ONLY | DONT_DELETE);
    constexpr PropertyConstness kConstness = PropertyConstness::kConst;
    DirectHandle<Object> kUndef = factory->undefined_value();
    for (DirectHandle<Name> name : sorted_names) {
      // Ensure unique names for transitions.
      DirectHandle<Name> unique_name = factory->InternalizeName(name);
      final_map = Map::TransitionToDataProperty(isolate, final_map,
                                                unique_name, kUndef, kAttrs,
                                                kConstness, StoreOrigin::kNamed);
    }

    // Precompute descriptor indices for direct writes during instance creation.
    // This eliminates runtime descriptor lookups.
    DirectHandle<FixedArray> descriptor_indices = factory->NewFixedArray(namec);
    {
      for (int i = 0; i < namec; ++i) {
        DirectHandle<Name> seek = original_names[i];
        int found_desc = -1;
        for (int j = 0; j < namec; ++j) {
          if (*seek == *sorted_names[j]) {  // internalized identity compare
            found_desc = j;
            break;
          }
        }
        // Should always find since we built sorted_names from original_names.
        if (found_desc < 0) found_desc = 0;
        descriptor_indices->set(i, Smi::FromInt(found_desc));
      }
    }

    // Precompute the shape hash seed from sorted key names.
    base::Hasher seed_hasher(0x9E3779B9);
    for (DirectHandle<Name> n : sorted_names) seed_hasher.AddHash(n->hash());
    uint32_t shape_seed = static_cast<uint32_t>(seed_hasher.hash());
    if (shape_seed == 0) shape_seed = 1;

    // Create a JSFunction whose [[Construct]] points to this builtin.
    DirectHandle<String> ctor_name = factory->empty_string();
    DirectHandle<SharedFunctionInfo> sfi =
        factory->NewSharedFunctionInfoForBuiltin(
            ctor_name, Builtin::kCompositeConstructor, /*len=*/1, kAdapt);
    sfi->set_language_mode(LanguageMode::kStrict);

    DirectHandle<JSFunction> ctor =
        Factory::JSFunctionBuilder{isolate, sfi, native_context}
            .set_map(isolate->strict_function_with_readonly_prototype_map())
            .Build();

    // Use the base (root) Composite initial map for the specialized ctor to
    // avoid using a transition map as initial map.
    DirectHandle<HeapObject> root_proto(base_map->prototype(), isolate);
    DirectHandle<JSObject> shared_proto(Cast<JSObject>(*root_proto), isolate);
    JSFunction::SetInitialMap(isolate, ctor, base_map, shared_proto);

    // Set function length to arity (number of declared fields).
    ctor->shared()->set_length(namec);

    // Create our precomputed data array.
    DirectHandle<FixedArray> meta_data = factory->NewFixedArray(3);
    meta_data->set(0, *final_map);  // Final map for instances
    meta_data->set(1, *descriptor_indices);  // Precomputed descriptor indices
    meta_data->set(2, Smi::FromInt(static_cast<int>(shape_seed)));  // Shape seed

    // Store metadata using a well-known key for fast access.
    DirectHandle<String> wrapper_key = factory->InternalizeUtf8String("__v8_composite_meta__");
    DirectHandle<FixedArray> meta_wrapper = factory->NewFixedArray(2);
    meta_wrapper->set(0, *wrapper_key);   // The key (for consistency)
    meta_wrapper->set(1, *meta_data);     // The actual metadata

    MaybeDirectHandle<Object> wrapper_result = JSObject::SetOwnPropertyIgnoreAttributes(
        ctor, wrapper_key, meta_wrapper,
        static_cast<PropertyAttributes>(DONT_ENUM | DONT_DELETE | READ_ONLY));
    if (wrapper_result.is_null()) {
      return ReadOnlyRoots(isolate).exception();
    }

    return *ctor;
  }

  // Specialized constructor path: fast instance creation using precomputed metadata.
  if (!target.is_identical_to(root_composite)) {
    // No subclassing support: require new_target === target for speed.
    if (*new_target != *target) {
      THROW_NEW_ERROR_RETURN_FAILURE(
        isolate, NewTypeError(MessageTemplate::kIncompatibleMethodReceiver));
    }

    // Fast metadata access: retrieve precomputed data from constructor property.
    DirectHandle<String> wrapper_key = isolate->factory()->InternalizeUtf8String("__v8_composite_meta__");
    DirectHandle<Object> wrapper_value;
    ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
        isolate, wrapper_value,
        JSReceiver::GetProperty(isolate, Cast<JSReceiver>(target), wrapper_key));

    if (!IsFixedArray(*wrapper_value)) {
      THROW_NEW_ERROR_RETURN_FAILURE(
          isolate, NewTypeError(MessageTemplate::kIncompatibleMethodReceiver));
    }

    Tagged<FixedArray> meta_wrapper = Cast<FixedArray>(*wrapper_value);
    if (meta_wrapper->length() < 2) {
      THROW_NEW_ERROR_RETURN_FAILURE(
          isolate, NewTypeError(MessageTemplate::kIncompatibleMethodReceiver));
    }

    Tagged<FixedArray> meta_data = Cast<FixedArray>(meta_wrapper->get(1));
    if (meta_data->length() < 3) {
      THROW_NEW_ERROR_RETURN_FAILURE(
          isolate, NewTypeError(MessageTemplate::kIncompatibleMethodReceiver));
    }

    // Extract precomputed metadata - no lookups, no iterations.
    Tagged<Map> final_map_tagged = Cast<Map>(meta_data->get(0));
    Tagged<FixedArray> descriptor_indices_tagged = Cast<FixedArray>(meta_data->get(1));
    uint32_t shape_seed = static_cast<uint32_t>(Smi::ToInt(meta_data->get(2)));

    DirectHandle<Map> final_map(final_map_tagged, isolate);
    DirectHandle<FixedArray> descriptor_indices(descriptor_indices_tagged, isolate);

    // Allocate with initial map, then migrate - this ensures proper initialization.
    DirectHandle<Map> initial_map(target->initial_map(), isolate);
    DirectHandle<JSComposite> composite = isolate->factory()->NewJSComposite(initial_map);

    // Migrate to the final map that has all properties pre-defined.
    JSObject::MigrateToMap(isolate, composite, final_map);

    // Initialize hasher with the precomputed shape seed.
    base::Hasher hasher(shape_seed);

    const int argc = descriptor_indices->length();

    // Direct field writes using precomputed indices - same loop for hashing and writing.
    DisallowGarbageCollection no_gc;
    Tagged<DescriptorArray> descriptors = final_map->instance_descriptors();
    for (int i = 0; i < argc; ++i) {
      DirectHandle<Object> value = args.atOrUndefined(isolate, i + 1);

      // Normalize zero values for consistent hashing.
      if (IsHeapNumber(*value) && Cast<HeapNumber>(*value)->value() == 0) {
        value = handle(Smi::FromInt(0), isolate);
      }

      // Hash the value for structural equality.
      if (IsJSComposite(*value)) {
        Tagged<JSComposite> nested_composite = Cast<JSComposite>(*value);
        Tagged<Smi> nested_hash = nested_composite->hashcode();
        hasher.AddHash(static_cast<uint32_t>(Smi::ToInt(nested_hash)));
      } else {
        Tagged<Smi> hash_smi = Object::GetOrCreateHash(*value, isolate);
        uint32_t value_hash = static_cast<uint32_t>(Smi::ToInt(hash_smi));
        hasher.AddHash(value_hash);
      }

      // Use precomputed descriptor index for direct field write.
      int desc_index = Smi::ToInt(descriptor_indices->get(i));
      InternalIndex field_index(desc_index);
      PropertyDetails details = descriptors->GetDetails(field_index);

      // Direct field write - maximum performance path.
      composite->WriteToField(field_index, details, *value);
    }

    // Make the instance non-extensible (immutable shape).
    Maybe<bool> result = JSReceiver::PreventExtensions(isolate, composite, kDontThrow);
    MAYBE_RETURN(result, ReadOnlyRoots(isolate).exception());

    uint32_t hashcode = static_cast<uint32_t>(hasher.hash());
    if (hashcode == 0) hashcode = 1;  // non-zero hash required by Map/Set
    composite->set_hashcode(hashcode);

    // Check cache for existing Composite with same hash - global interning
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

  // Root constructor called directly without being a factory first.
  // This is not supported - user must use the two-phase construction pattern.
  THROW_NEW_ERROR_RETURN_FAILURE(
      isolate, NewTypeError(MessageTemplate::kCalledOnNonObject,
                            isolate->factory()->NewStringFromAsciiChecked(
                                "Composite must be used as a factory: new Composite('field1', 'field2')")));
}

}  // namespace internal
}  // namespace v8
