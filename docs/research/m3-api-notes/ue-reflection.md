# UE 5.8.1 Reflection API — verified ground truth for the data-binding milestone

**Engine tree read:** `/w/Unreal/UnrealEngine` — `Engine/Build/Build.version` reports
`MajorVersion 5 / MinorVersion 8 / PatchVersion 1 / BranchName "UE5"`.

**Method.** Every `file:line` below was opened in this tree. Paths are relative to
`/w/Unreal/UnrealEngine/Engine/Source/` unless written out in full. Where a statement is an
inference rather than a quote it is tagged **[inference]**. Where source alone cannot settle the
question, the experiment that would is named.

Two answers up front, because they decide the architecture:

* **§5 — `UPROPERTY` metadata does NOT exist in a packaged non-editor build.** The whole
  `HasMetaData`/`GetMetaData` API is inside `#if WITH_METADATA`, and `WITH_METADATA` is `#define`d
  to `WITH_EDITORONLY_DATA`, which UBT sets to `0` for Game/Client/Server targets. Use
  `EPropertyFlags` (which are cooked) instead.
* **§6 — The *builder* (walking `UStruct` → property description) can run off the game thread for
  types that are already loaded; the *per-frame value read* cannot, without the game thread
  cooperating.** Type descriptors are effectively immutable post-`Link()` and native ones are GC
  roots; instance data has no engine-provided synchronisation at all.

---

## 1. Enumerating properties

### 1.1 The two lists on `UStruct`

```cpp
// Runtime/CoreUObject/Public/UObject/Class.h:517-522
    /** Pointer to start of linked list of child fields */
    UPROPERTY(SkipSerialization)
    TObjectPtr<UField> Children;

    /** Pointer to start of linked list of child fields */
    FField* ChildProperties;
```

```cpp
// Runtime/CoreUObject/Public/UObject/Class.h:548-555
    /** In memory only: Linked list of properties from most-derived to base */
    FProperty* PropertyLink;
    /** In memory only: Linked list of object reference properties from most-derived to base */
    FProperty* RefLink;
    /** In memory only: Linked list of properties requiring destruction. ... */
    FProperty* DestructorLink;
    /** In memory only: Linked list of properties requiring post constructor initialization */
    FProperty* PostConstructLink;
```

* `Children` (`UField*`, linked by `UField::Next`) holds **`UFunction`s and `UEnum`s/`UStruct`s** —
  not properties. `ChildProperties` (`FField*`, linked by `FField::Next`) holds this struct's **own**
  `FProperty`s only. `TFieldIterator<T>` picks the right head via a template specialisation:

```cpp
// Runtime/CoreUObject/Public/UObject/UnrealType.h:7176-7186
template <>
inline UField* GetChildFieldsFromStruct(const UStruct* Owner)
{
    return Owner->Children;
}

template <>
inline FField* GetChildFieldsFromStruct(const UStruct* Owner)
{
    return Owner->ChildProperties;
}
```

* `PropertyLink` is a **flattened** list (this struct's properties followed by its supers'), built
  during `UStruct::Link` by walking `TFieldIterator<FProperty>` and appending in iteration order:

```cpp
// Runtime/CoreUObject/Private/UObject/Class.cpp:1036, 1042, 1096, 1099
    UEProperty_Private::FPropertyListBuilderPropertyLink PropertyLinkBuilder(&PropertyLink);
    ...
    for (TFieldIterator<FProperty> It(this); It; ++It)
    {
        ...
        PropertyLinkBuilder.AppendNoTerminate(*Property);
    }
    PropertyLinkBuilder.NullTerminate();
```

  So `for (FProperty* P = Struct->PropertyLink; P; P = P->PropertyLinkNext)` yields **exactly the
  same sequence** as `TFieldIterator<FProperty>(Struct)` with default flags — including super-class
  properties and including deprecated ones. It is cheaper (no per-node cast-flag test, no
  struct-chain walk), and it is what to use in a hot loop. It is *not* configurable: you cannot ask
  `PropertyLink` for "own properties only".

### 1.2 `TFieldIterator` / `TFieldRange`

```cpp
// Runtime/CoreUObject/Public/UObject/UnrealType.h:7134-7145
enum class EFieldIterationFlags : uint8
{
    None = 0,
    IncludeSuper = 1<<0,        // Include super class
    IncludeDeprecated = 1<<1,   // Include deprecated properties
    IncludeInterfaces = 1<<2,   // Include interfaces

    IncludeAll = IncludeSuper | IncludeDeprecated | IncludeInterfaces,

    Default = IncludeSuper | IncludeDeprecated,
};
```

```cpp
// Runtime/CoreUObject/Public/UObject/UnrealType.h:7210
TFieldIterator(const UStruct* InStruct, EFieldIterationFlags InIterationFlags = EFieldIterationFlags::Default)
```

`TFieldRange<T>` (`UnrealType.h:7332-7353`) is the range-for wrapper with the same two constructors.

Canonical shapes:

```cpp
// A UScriptStruct, own properties only (a USTRUCT has no super in the common case anyway):
for (const FProperty* P : TFieldRange<FProperty>(ScriptStruct, EFieldIterationFlags::None)) { ... }

// A UClass, this class + all bases, skipping deprecated:
for (const FProperty* P : TFieldRange<FProperty>(Class, EFieldIterationFlags::IncludeSuper)) { ... }

// Everything the engine considers default (super + deprecated, no interfaces):
for (const FProperty* P : TFieldRange<FProperty>(Class)) { ... }
```

**Deprecated properties are included by default** (`Default = IncludeSuper | IncludeDeprecated`,
`UnrealType.h:7143`). The filter is applied in `IterateToNext`:

```cpp
// Runtime/CoreUObject/Public/UObject/UnrealType.h:7286-7293
                if (FieldClass->HasAllCastFlags(CASTCLASS_FProperty))
                {
                    FProperty* Prop = (FProperty*)CurrentField;
                    if (Prop->HasAllPropertyFlags(CPF_Deprecated) && !bIncludeDeprecated)
                    {
                        continue;
                    }
                }
```

Super traversal uses `GetInheritanceSuper()`, which is `GetSuperStruct()` for `UStruct`/
`UScriptStruct`/`UClass` (`Class.h:680`) but **`nullptr` for `UFunction`** (`Class.h:2676`), so a
`UFUNCTION`'s parameter list never bleeds into a parent's.

### 1.3 Order — and how stable it is

Verified chain for a native C++ type:

1. UHT emits `PropPointers[]` **in header declaration order**. Checked against a real generated
   file: `Engine/Intermediate/Build/Linux/UnrealEditor/Inc/MovieSceneTools/UHT/MovieSceneToolsUserSettings.gen.cpp:171-177`
   lists `bDrawThumbnails, bDrawSingleThumbnails, ThumbnailSize, Quality_Underlying, Quality`, and
   `Editor/MovieSceneTools/Public/MovieSceneToolsUserSettings.h:35,39,43,47` declares
   `bDrawThumbnails, bDrawSingleThumbnails, ThumbnailSize, Quality` in that order.
2. `ConstructFProperties` walks that array **backwards**:
   ```cpp
   // Runtime/CoreUObject/Private/UObject/UObjectGlobals.cpp:6499-6507
   void ConstructFProperties(UObject* Outer, const FPropertyParamsBase* const* PropertyArray, int32 NumProperties)
   {
       // Move pointer to the end, because we'll iterate backwards over the properties
       PropertyArray += NumProperties;
       while (NumProperties)
       {
           ConstructFProperty(Outer, PropertyArray, NumProperties);
       }
   }
   ```
3. Each new property **prepends** itself to the owner:
   ```cpp
   // Runtime/CoreUObject/Private/UObject/Property.cpp:826, 834-838
   void FProperty::Init()
   {
       ...
       if (GetOwner<UObject>())
       {
           UField* OwnerField = GetOwnerChecked<UField>();
           OwnerField->AddCppProperty(this);
   ```
   ```cpp
   // Runtime/CoreUObject/Private/UObject/Class.cpp:723-727
   void UStruct::AddCppProperty(FProperty* Property)
   {
       Property->Next = ChildProperties;
       ChildProperties = Property;
   }
   ```
   (The `UPropertyBag` code comments on the same behaviour: *"Add properties (AddCppProperty() adds
   them backwards in the linked list)"* — `Runtime/CoreUObject/Private/StructUtils/PropertyBag.cpp:4176`.)

**Conclusion:** `ChildProperties` head-to-tail == C++ declaration order. With `IncludeSuper`, the
most-derived struct's properties come **first** and base-class properties **last**.

Stability:

* **Across runs of the same binary: stable.** The order derives from a `static constexpr` array
  address order, not from hashing or object-index order. **[inference, but tightly constrained by
  the three quotes above]**
* **Across builds: not stable.** Reordering `UPROPERTY` declarations in the header reorders the
  list. Offsets also move.
* **Blueprint types: not stable across recompiles.** A `UUserDefinedStruct`/`UBlueprintGeneratedClass`
  is rebuilt from its editor data on every compile.

⇒ **Never persist a property index.** Key a wire model by `FProperty::GetFName()` (or, for
Blueprint types, by GUID — §7.4).

### 1.4 Enum properties add an extra entry that is *not* a sibling

In the generated file above, `PropPointers` contains both `NewProp_Quality_Underlying` and
`NewProp_Quality`. The underlying integer property is constructed as a **child of the enum
property**, not of the struct:

```cpp
// Runtime/CoreUObject/Private/UObject/UObjectGlobals.cpp:6402-6409, 6492-6495
            case EPropertyGenFlags::Enum:
            {
                NewProp = NewFProperty<FEnumProperty, FEnumPropertyParams>(Outer, *PropBase);

                // Next property is the underlying integer property
                ReadMore = 1;
            }
            break;
        ...
        for (; ReadMore; --ReadMore)
        {
            ConstructFProperty(NewProp, PropertyArray, NumProperties);
        }
```

Same mechanism for `EPropertyGenFlags::Optional` (`UObjectGlobals.cpp:6417-6424`). So
`TFieldIterator` over a struct never surfaces these inner properties — good. (`TAllFieldsIterator`
in `Runtime/CoreUObject/Public/UObject/FieldIterator.h:37-190` is the thing that *does* descend into
inners; you do not want it here.)

### 1.5 Sizes

```cpp
// Runtime/CoreUObject/Public/UObject/Class.h:784, 796   (UStruct::GetPropertiesSize, UScriptStruct::GetStructureSize)
// Runtime/CoreUObject/Public/UObject/UnrealType.h:1208-1211
    UE_FORCEINLINE_HINT int32 GetSize() const
    {
        return ArrayDim * GetElementSize();
    }
```

`FProperty::GetElementSize()` (`UnrealType.h:293-298`) is the accessor; the raw `ElementSize` member
is `UE_DEPRECATED(5.5, "Use GetElementSize/SetElementSize instead.")` (`UnrealType.h:179-180`).

---

## 2. The `FProperty` hierarchy

### 2.1 Detection is a bit test, not RTTI

```cpp
// Runtime/CoreUObject/Public/UObject/Field.h:765-775
    template<typename T>
    bool IsA() const
    {
        if constexpr (!!(T::StaticClassCastFlagsPrivate()))
        {
            return !!(GetCastFlags() & T::StaticClassCastFlagsPrivate());
        }
        else
        {
            return GetClass()->IsChildOf(T::StaticClass());
        }
```

```cpp
// Runtime/CoreUObject/Public/UObject/Field.h:1126-1131
template<typename FieldType>
UE_FORCEINLINE_HINT FieldType* CastField(FField* Src)
{
    return Src && Src->IsA<FieldType>() ? static_cast<FieldType*>(Src) : nullptr;
}
```

So `CastField<FIntProperty>(P)` is one `and` + one branch. `ExactCastField<T>` (`Field.h:1138-1148`)
compares `GetClass()` pointers exactly — use it only when you deliberately want to reject subclasses.
`CastFieldChecked<T>` (`Field.h:1151-1175`) `checkf`s on failure and compiles to a plain C cast when
`DO_CHECK` is off.

Cast flags (`Runtime/CoreUObject/Public/UObject/ObjectMacros.h`): `CASTCLASS_FProperty` 362,
`CASTCLASS_FNumericProperty` 371, `CASTCLASS_FBoolProperty` 364, `CASTCLASS_FStrProperty` 361,
`CASTCLASS_FTextProperty` 377, `CASTCLASS_FNameProperty` 360, `CASTCLASS_FEnumProperty` 395,
`CASTCLASS_FStructProperty` 367, `CASTCLASS_FArrayProperty` 368, `CASTCLASS_FMapProperty` 393,
`CASTCLASS_FSetProperty` 394, `CASTCLASS_FObjectPropertyBase` 373, `CASTCLASS_FObjectProperty` 363,
`CASTCLASS_FWeakObjectProperty` 374, `CASTCLASS_FSoftObjectProperty` 376, `CASTCLASS_FClassProperty`
357, `CASTCLASS_FDelegateProperty` 370, `CASTCLASS_FOptionalProperty` 403,
`CASTCLASS_FUtf8StrProperty` 407, `CASTCLASS_FAnsiStrProperty` 408.

**Dispatch order matters.** `FClassProperty` *is a* `FObjectProperty` (`UnrealType.h:3465`) which
*is a* `FObjectPropertyBase`; `FByteProperty`, `FIntProperty`, … all satisfy
`CastField<FNumericProperty>`. Test the most-derived kind first, or switch on
`P->GetClass()` identity.

### 2.2 Table

`UnrealType.h` unless noted. "Storage" = what actually lives at
`ContainerPtrToValuePtr<void>(Container)`.

| Property class | Decl | Storage | Read generically |
|---|---|---|---|
| `FNumericProperty` (abstract) | 1785 | see concrete | `GetSignedIntPropertyValue` / `GetUnsignedIntPropertyValue` / `GetFloatingPointPropertyValue` (1886-1902) |
| `FByteProperty` | 2190 | `uint8`; **may carry a `UEnum`** via `Enum` member (2195) and `GetIntPropertyEnum()` (2253) | numeric getters, then `GetIntPropertyEnum()` |
| `FInt8/16/Property`, `FIntProperty`, `FInt64Property` | 2267/2301/2336/2370 | `int8/int16/int32/int64` | `GetSignedIntPropertyValue` |
| `FUInt16/32/64Property` | 2404/2438/2472 | `uint16/uint32/uint64` | `GetUnsignedIntPropertyValue` |
| `FFloatProperty` / `FDoubleProperty` | 2507 / 2547 | `float` / `double` | `GetFloatingPointPropertyValue` |
| `FBoolProperty` | 2589 | **see §2.3** | `GetPropertyValue_InContainer` (2688) |
| `FStrProperty` | `StrProperty.h` → `StrProperty.h.inl:28` | `FString` | `GetPropertyValue_InContainer` |
| `FUtf8StrProperty` / `FAnsiStrProperty` | `Utf8StrProperty.h` / `AnsiStrProperty.h` | `FUtf8String` / `FAnsiString` | ditto — **separate cast flags, `CastField<FStrProperty>` misses them** |
| `FTextProperty` | `TextProperty.h:20` | `FText` | `GetPropertyValue_InContainer` |
| `FNameProperty` | 3723 | `FName` | `GetPropertyValue_InContainer` |
| `FEnumProperty` | `EnumProperty.h:29` | the underlying integer, size = `GetUnderlyingProperty()->GetElementSize()` | `GetUnderlyingProperty()->GetSignedIntPropertyValue(Ptr)`, then `GetEnum()` |
| `FStructProperty` | 6387 | the struct's bytes, inline | recurse on `Struct` (`TObjectPtr<UScriptStruct> Struct`, 6392) |
| `FArrayProperty` | 3776 | `FScriptArray` | `FScriptArrayHelper` + `Inner` (3782) |
| `FMapProperty` | 3920 | `FScriptMap` | `FScriptMapHelper` + `KeyProp`/`ValueProp` (3925-3926) |
| `FSetProperty` | 4110 | `FScriptSet` | `FScriptSetHelper` + `ElementProp` (4115) |
| `FObjectProperty` | 3135 | `TObjectPtr<UObject>` | `GetObjectPropertyValue_InContainer` (3191) |
| `FWeakObjectProperty` | 3252 | `FWeakObjectPtr` | `GetObjectPropertyValue*` (returns null if stale) |
| `FLazyObjectProperty` | 3319 | `FLazyObjectPtr` | ditto |
| `FSoftObjectProperty` | 3383 | `FSoftObjectPtr` | `GetObjectPropertyValue` = currently-resolved or null; **`LoadObjectPropertyValue` (3435) synchronously loads** |
| `FClassProperty` | 3465 | `TObjectPtr<UObject>` (a `UClass*`) | as `FObjectProperty`; `MetaClass` (3470) is the constraint |
| `FSoftClassProperty` | 3550 | `FSoftObjectPtr` | as `FSoftObjectProperty` |
| `FInterfaceProperty` | 3631 | `FScriptInterface` | — |
| `FDelegateProperty` | 6487 | `FScriptDelegate`; `SignatureFunction` (6492) | not a value; skip |
| `FMulticastInlineDelegateProperty` / `…Sparse…` | 6693 / 6748 | `FMulticastScriptDelegate` / `FSparseDelegate` | skip |
| `FOptionalProperty` | `PropertyOptional.h:184` | value + (maybe intrusive) set-flag | `IsIntrusiveOptionalValueSet` / layout API |
| `FFieldPathProperty` | `FieldPathProperty.h:24` | `FFieldPath` | skip |

`FObjectPropertyBase::PropertyClass` (`UnrealType.h:2780`) is the declared pointee class for every
object-ish kind.

### 2.3 `FBoolProperty` — the bitfield trap

Storage is **not** `bool`. It is *the whole integer that contains the bit*, plus a mask:

```cpp
// Runtime/CoreUObject/Public/UObject/UnrealType.h:2595-2603
private:

    /** Size of the bitfield/bool property. Equal to ElementSize but used to check if the property has been properly initialized (0-8, where 0 means uninitialized). */
    uint8 FieldSize;
    /** Offset from the memeber variable to the byte of the property (0-7). */
    uint8 ByteOffset;
    /** Mask of the byte with the property value. */
    uint8 ByteMask;
    /** Mask of the field with the property value. Either equal to ByteMask or 255 in case of 'bool' type. */
    uint8 FieldMask;
```

```cpp
// Runtime/CoreUObject/Public/UObject/UnrealType.h:2682-2691
    inline bool GetPropertyValue(void const* A) const
    {
        check(FieldSize != 0);
        uint8* ByteValue = (uint8*)A + ByteOffset;
        return !!(*ByteValue & FieldMask);
    }
    UE_FORCEINLINE_HINT bool GetPropertyValue_InContainer(void const* A, int32 ArrayIndex = 0) const
    {
        return GetPropertyValue(ContainerPtrToValuePtr<void>(A, ArrayIndex));
    }
```

For a native `bool`, `FieldMask == 0xFF` and `ByteOffset == 0`; for a bitfield
(`uint8 bFoo : 1;`) the mask isolates one bit and `ByteOffset` selects the byte within the storage
integer. Set by `SetBoolSize`:

```cpp
// Runtime/CoreUObject/Private/UObject/PropertyBool.cpp:80-91
    ByteOffset = 0;
    if (bIsNativeBool)
    {
        ByteMask = true;
        FieldMask = 255;
    }
    else
    {
        // Calculate ByteOffset and get ByteMask.
        for (ByteOffset = 0; ByteOffset < InSize && ((ByteMask = *((uint8*)&TestBitmask + ByteOffset)) == 0); ByteOffset++);
        FieldMask = ByteMask;
    }
```

Consequences you must respect:

1. **`*ContainerPtrToValuePtr<bool>(Obj)` is wrong for bitfields.** It reads the first byte of the
   storage integer, which contains up to eight unrelated bits. Always
   `BoolProp->GetPropertyValue_InContainer(Obj)`.
2. **`Offset_Internal` for a bitfield points at the storage integer, and `GetElementSize()` is that
   integer's size** — two bitfields declared adjacently share an offset and a size. So a "one
   property, one slot" scratch buffer keyed on `(Offset, ElementSize)` will alias.
3. **`CopySingleValue`/`CopyCompleteValue` on a bitfield read-modify-write the destination byte:**
   ```cpp
   // Runtime/CoreUObject/Private/UObject/PropertyBool.cpp:442-451
   void FBoolProperty::CopyValuesInternal( void* Dest, void const* Src, int32 Count  ) const
   {
       check(FieldSize != 0 && !IsNativeBool());
       for (int32 Index = 0; Index < Count; Index++)
       {
           uint8* DestByteValue = (uint8*)Dest + Index * GetElementSize() + ByteOffset;
           uint8* SrcByteValue = (uint8*)Src + Index * GetElementSize() + ByteOffset;
           *DestByteValue = (*DestByteValue & ~FieldMask) | (*SrcByteValue & FieldMask);
       }
   }
   ```
   Copying into freshly-`Malloc`'d scratch leaves the non-masked bits **uninitialised**, so a
   `memcmp`-based diff of that scratch produces false positives. Copy bools as `bool`, not as bytes.
   (`SetBoolSize` clears `CPF_IsPlainOldData | CPF_ZeroConstructor` for bitfields —
   `PropertyBool.cpp:72-76` — which is what routes `CopyCompleteValue` to `CopyValuesInternal` at
   `UnrealType.h:915-928`.)
4. `FBoolProperty::Identical` is correct and mask-aware (`PropertyBool.cpp:416-422`), so
   *comparison* via `Identical` is safe even though byte comparison is not.
5. **Documentation bug in the engine header** — `UnrealType.h:2739` says of `GetByteOffset()`
   *"Only valid if IsNativeBool() is true."* That is backwards: `SetBoolSize` sets `ByteOffset = 0`
   for native bools and *computes* it for bitfields (`PropertyBool.cpp:80-91`). Do not trust that
   comment; trust the loop.

---

## 3. Generic value access, with costs

### 3.1 `ContainerPtrToValuePtr` — pure address arithmetic, **cheapest**

```cpp
// Runtime/CoreUObject/Public/UObject/UnrealType.h:733-745
    inline void* ContainerVoidPtrToValuePtrInternal(void* ContainerPtr, int32 ArrayIndex) const
    {
        checkf((ArrayIndex >= 0) && (ArrayIndex < ArrayDim), ...);
        check(ContainerPtr);
        ...
        return (uint8*)ContainerPtr + Offset_Internal + static_cast<size_t>(GetElementSize()) * ArrayIndex;
    }
```

Four public overloads at `UnrealType.h:800-819`, all `UE_FORCEINLINE_HINT`. Zero allocation, no
virtual call.

**Overload trap:** the `UObject*` overload routes to `ContainerUObjectPtrToValuePtrInternal`
(`UnrealType.h:747-772`), which in a `DO_CHECK` build runs four `checkf`s including
`((UObject*)ContainerPtr)->IsA(GetOwner<UClass>())`. The `void*` overload does none of that. Passing
a `UObject*` through a `void*` (or vice versa) silently skips or adds those checks — the header says
so at `UnrealType.h:793-799`: *"You can \_only\_ call this function on a UObject\* or a uint8\*.
If the property you want is a 'top level' UObject property, you \_must\_ call the function passing in
a UObject\* and not a uint8\*."* For a hot per-frame path, cache the `void*` form.

**Getter/setter trap:** `ContainerPtrToValuePtr` bypasses `UPROPERTY(Getter=…)`. Native getters are
declared via `TPropertyWithSetterAndGetter`
(`Runtime/CoreUObject/Public/UObject/PropertyWithSetterAndGetter.h:31-56`) and surface as
`FProperty::HasGetter()` (`UnrealType.h:355-358`). If a property has a getter, the raw memory may not
be the value the game logic considers authoritative. Check `HasGetter()` once at build time and route
those properties through `GetValue_InContainer`.

### 3.2 `GetValue_InContainer` / `SetValue_InContainer` — **allocating for non-POD**

```cpp
// Runtime/CoreUObject/Public/UObject/UnrealType.h:635-656
    inline void SetValue_InContainer(void* OutContainer, const void* InValue) const
    {
        if (!HasSetter())
        {
            CopyCompleteValue(ContainerVoidPtrToValuePtrInternal(OutContainer, 0), InValue);
        }
        else
        {
            CallSetter(OutContainer, InValue);
        }
    }
    inline void GetValue_InContainer(void const* InContainer, void* OutValue) const
    {
        if (!HasGetter())
        {
            CopyCompleteValue(OutValue, ContainerVoidPtrToValuePtrInternal((void*)InContainer, 0));
        }
        else
        {
            CallGetter(InContainer, OutValue);
        }
    }
```

Cost = cost of `CopyCompleteValue` for that type: `memcpy` when `CPF_IsPlainOldData`
(`UnrealType.h:915-928`), otherwise a virtual `CopyValuesInternal` — which for `FStrProperty` is an
`FString` assignment (**heap allocation per call**), for `FArrayProperty`/`FMapProperty` a full deep
copy. `OutValue` must already be a constructed object of the right type.

`Get/SetSingleValue_InContainer` (`UnrealType.h:664-672`, impl
`Runtime/CoreUObject/Private/UObject/Property.cpp:1326-1380`) add fixed-array indexing; when a
getter is present *and* `ArrayDim > 1` they `AllocateAndInitializeValue()` +
`DestroyAndFreeValue()` — i.e. **one malloc/free per read** (`Property.cpp:1300-1317`).

### 3.3 Text export/import — **always allocates and formats; never on a per-frame path**

The old `FProperty::ExportTextItem(...)` name is gone in 5.8. The current surface:

```cpp
// Runtime/CoreUObject/Public/UObject/UnrealType.h:587-595
    void ExportTextItem_Direct(FString& ValueStr, TNotNull<const void*> PropertyValue, const void* DefaultValue, UObject* Parent, int32 PortFlags, UObject* ExportRootScope = nullptr) const
    {
        ExportText_Internal(ValueStr, PropertyValue, EPropertyPointerType::Direct, DefaultValue, Parent, PortFlags, ExportRootScope);
    }

    void ExportTextItem_InContainer(FString& ValueStr, TNotNull<const void*> Container, const void* DefaultValue, UObject* Parent, int32 PortFlags, UObject* ExportRootScope = nullptr) const
    {
        ExportText_Internal(ValueStr, Container, EPropertyPointerType::Container, DefaultValue, Parent, PortFlags, ExportRootScope);
    }
```

Import counterparts: `ImportText_InContainer` (`UnrealType.h:606`), `ImportText_Direct`
(`UnrealType.h:625`). The virtuals `ExportText_Internal` / `ImportText_Internal` are `protected`
(`UnrealType.h:718-721`) — you call the four public wrappers.

**Silent-failure trap.** There is a *second*, differently-named pair that looks like the same thing
and is not:

```cpp
// Runtime/CoreUObject/Private/UObject/Property.cpp:1021-1041
bool FProperty::ExportText_Direct(FString& ValueStr, TNotNull<const void*> Data, const void* Delta, UObject* Parent, int32 PortFlags, UObject* ExportRootScope) const
{
    ...
    if( Data==Delta ||
        ... ||
        (!bExportOverride && !Identical(Data,Delta,PortFlags)) )
    {
        ExportText_Internal(...);
        return true;
    }

    return false;
}
```

`ExportText_Direct` / `ExportText_InContainer` (`UnrealType.h:725-729`) **write nothing and return
`false` when the value matches `Delta`**. And `Identical(A, nullptr)` means *"A equals the type's
default"*:

```cpp
// Runtime/CoreUObject/Public/UObject/UnrealType.h:1767-1774
        if (B)
        {
            return TTypeFundamentals::GetPropertyValue(A) == TTypeFundamentals::GetPropertyValue(B);
        }
        else
        {
            return TTypeFundamentals::GetPropertyValue(A) == TTypeFundamentals::GetDefaultPropertyValue();
        }
```

So `ExportText_Direct(Str, Value, /*Delta=*/nullptr, …)` produces an **empty string for every
default-valued property** — zero ints, empty arrays, empty strings — with no error. If you want the
text unconditionally, use `ExportTextItem_Direct` / `ExportTextItem_InContainer`.

Cost: every implementation appends to an `FString`. `FNumericProperty::ExportText_Internal`
(`Runtime/CoreUObject/Private/UObject/PropertyNumeric.cpp:164-174`) calls
`GetNumericPropertyValueToString`, which returns an `FString` by value.
`FBoolProperty::ExportText_Internal` does `ValueStr += FString::Printf(TEXT("%s"), Temp)`
(`PropertyBool.cpp:363-364`) — a `Printf` to emit a literal. `FStructProperty` recurses per member
and compares each against defaults. Treat the whole family as **cold path only** (debug dumps,
one-time serialisation), never per-frame.

### 3.4 Container helpers

`FScriptArrayHelper` (`UnrealType.h:4262`):

```cpp
// Runtime/CoreUObject/Public/UObject/UnrealType.h:4285-4288
    UE_FORCEINLINE_HINT FScriptArrayHelper(const FArrayProperty* InProperty, const void* InArray)
        : FScriptArrayHelper(Internal, InProperty->Inner, InArray, InProperty->Inner->GetElementSize(), InProperty->Inner->GetMinAlignment(), InProperty->ArrayFlags)
    {
    }
```

The private ctor (`UnrealType.h:4543-4557`) just stores five fields — but `GetMinAlignment()` is a
**virtual call**, so constructing the helper is not free-free. Hoist it out of inner loops.
`Num()` (4303-4308) and `GetRawPtr(Index)` (4324-4333) are pointer arithmetic behind a
lambda-dispatched `WithScriptArray` on the freezable/heap allocator flag. `FScriptArrayHelper_InContainer`
(4651-4663) is the `ContainerPtrToValuePtr` convenience wrapper.

`FScriptMapHelper` (`UnrealType.h:4759`) and `FScriptSetHelper` (`UnrealType.h:5763`) are sparse:
`Num()` (4824 / 5817) is the element count but `GetMaxIndex()` (4848 / 5840) is the slot count, and
`IsValidIndex(i)` (4814 / 5807) filters. Iterate with the provided iterator, not `0..Num()`:

```cpp
// Runtime/CoreUObject/Public/UObject/UnrealType.h:4795-4805
    using FIterator = TScriptContainerIterator<FScriptMapHelper>;

    FIterator CreateIterator() const
    ...
```

The header spells the hazard out at `UnrealType.h:4746-4758` (*"map can contain invalid entries
some number of valid entries (i.e. Num() ) can be smaller that the actual number of elements (i.e.
GetMaxIndex() )"*). `GetPairPtr` / `GetKeyPtr` / `GetValuePtr` (4865, 4901, 4923) `checkf` on an
invalid internal index — so a naive `for (i = 0; i < Num(); ++i) GetKeyPtr(i)` on a map that has had
removals will **assert**, not silently misbehave. Values are at
`GetKeyPtr(i) + MapLayout.ValueOffset`.

Helpers are pointer views; they allocate nothing on construction and do not copy the container.

### 3.5 Cost summary

| Call | Allocates? | Virtual? | Verdict |
|---|---|---|---|
| `ContainerPtrToValuePtr` | no | no | per-frame ✅ |
| `FBoolProperty::GetPropertyValue_InContainer` | no | no | per-frame ✅ |
| `FNumericProperty::Get*PropertyValue` | no | **yes** | per-frame ✅ (one indirect call) |
| `FObjectPropertyBase::GetObjectPropertyValue_InContainer` | no | yes | per-frame ✅ |
| `FScriptArray/Map/SetHelper` ctor | no | yes (`GetMinAlignment`) | hoist |
| `CopySingleValue` / `CopyCompleteValue` | POD: no; `FString`/containers: **yes** | POD: no | per-frame only for POD |
| `Get/SetValue_InContainer` | same as above; +malloc if getter && `ArrayDim>1` | yes | prefer direct read when `!HasGetter()` |
| `GetSingleValue_InContainer` | +malloc when getter && `ArrayDim>1` | yes | avoid in loops |
| `ExportTextItem_*` / `ImportText_*` | **yes, always** | yes | cold path only |
| `AllocateAndInitializeValue` | **yes** (`MallocZeroed`, `Property.cpp:1300-1308`) | yes | setup only |

---

## 4. Change detection

### 4.1 What exists

**`FProperty::Identical`** — pure virtual, per-type:

```cpp
// Runtime/CoreUObject/Public/UObject/UnrealType.h:517
    virtual bool Identical(TNotNull<const void*> A, const void* B, uint32 PortFlags = 0) const PURE_VIRTUAL(FProperty::Identical, return false;);
```

```cpp
// Runtime/CoreUObject/Public/UObject/UnrealType.h:528-531
    bool Identical_InContainer(TNotNull<const void*> A, const void* B, int32 ArrayIndex = 0, uint32 PortFlags = 0) const
    {
        return Identical(ContainerPtrToValuePtr<void>(A, ArrayIndex), B ? ContainerPtrToValuePtr<void>(B, ArrayIndex) : nullptr, PortFlags);
    }
```

Note the 5.8 signature: `A` is `TNotNull<const void*>`, `B` is a plain `const void*` and **`B == nullptr`
means "compare against the type default"** (`UnrealType.h:1767-1774`, quoted in §3.3).

**Copy** — `CopySingleValue` (`UnrealType.h:881-894`) and `CopyCompleteValue`
(`UnrealType.h:915-928`), both taking the POD fast path when `CPF_IsPlainOldData`, plus
`CopyCompleteValue_InContainer` (`UnrealType.h:929-932`).

**Hash** — `FProperty::GetValueTypeHash(const void* Src)` (`UnrealType.h:899`), valid only when
`CPF_HasGetValueTypeHash` is set (`ObjectMacros.h:485`).

### 4.2 What does not exist

There is **no built-in "has this property changed since last frame"** on `FProperty`, `UStruct`, or
`UObject`. Every changed-value system in the engine hand-rolls a shadow buffer:

* **Replication** — `FRepLayout` keeps a shadow copy and diffs per command:
  ```cpp
  // Runtime/Engine/Private/RepLayout.cpp:668-753 (excerpt)
  static FORCEINLINE bool PropertiesAreIdenticalNative(
      const FRepLayoutCmd& Cmd, const void* A, const void* B, ...)
  {
      switch (Cmd.Type)
      {
          case ERepLayoutCmdType::PropertyBool:       return CompareBool(Cmd, A, B);
          case ERepLayoutCmdType::PropertyNativeBool: return CompareValue<bool>(A, B);
          case ERepLayoutCmdType::PropertyInt:        return CompareValue<int32>(A, B);
          ...
          case ERepLayoutCmdType::Property:           return Cmd.Property->Identical(A, B);
  ```
  This is the pattern to copy: a **flat, pre-resolved command list** with a typed comparator per
  entry, falling back to the virtual `Identical` only for kinds you didn't special-case.
  Its `Cmd.Offset`s are absolute into the shadow/object buffers, so no per-property virtual dispatch
  is needed to *locate* the value. Even `PropertyBool` delegates to `Cmd.Property->Identical`
  (`RepLayout.cpp:588-594`) — Epic did not hand-roll the mask compare.

* **FieldNotify** — an *opt-in, runtime, non-editor* push notification, not a diff:
  ```cpp
  // Runtime/FieldNotification/Public/INotifyFieldValueChanged.h:32-48
      /** Add a delegate that will be notified when the FieldId is value changed. */
      virtual FDelegateHandle AddFieldValueChangedDelegate(UE::FieldNotification::FFieldId InFieldId, FFieldValueChangedDelegate InNewDelegate) = 0;
      ...
      /** Broadcast to the registered delegate that the FieldId value changed. */
      virtual void BroadcastFieldValueChanged(UE::FieldNotification::FFieldId InFieldId) = 0;
  ```
  Enabled by `UPROPERTY(FieldNotify)` / `UFUNCTION(FieldNotify)` (`ObjectMacros.h:1057` and `1212`).
  The setter must call `BroadcastFieldValueChanged` — it is not automatic for direct writes. Usable
  only on classes that implement `UNotifyFieldValueChanged`.

* **RepNotify** — `CPF_RepNotify` (`ObjectMacros.h:466`) + `FProperty::RepNotifyFunc`
  (`UnrealType.h:240`); fires on the *receiving* side of replication only.

### 4.3 `PostEditChangeProperty` is editor-only

`UObject::PreEditChange`, `PostEditChangeProperty`, `PostEditChangeChainProperty` are all inside a
single `#if WITH_EDITOR` block:
`Runtime/CoreUObject/Public/UObject/Object.h:425` (`#if WITH_EDITOR`) … `473` (`PostEditChangeProperty`)
… `479` (`PostEditChangeChainProperty`) … `520` (`#endif // WITH_EDITOR`).

⇒ **Nothing in that family exists in a packaged game.** The runtime alternatives are FieldNotify
(opt-in, push) and RepNotify (replication only). Otherwise: poll and diff.

### 4.4 Cheapest correct diff, per kind

Cost of the generic path, measured against source:

* Numerics / `FName` / `FStrProperty` / `FNameProperty` — `TProperty_WithEqualityAndSerializer::Identical`
  (`UnrealType.h:1760-1775`) is `operator==` on the C++ type. Cheapest correct diff: read the POD /
  `FName` index into a shadow slot and compare integers. `FString` requires a length+memcmp; keep a
  shadow `FString` and use `operator==`, or shadow a hash and accept collisions.
* `FBoolProperty` — `Identical` masks correctly (`PropertyBool.cpp:416-422`). Cheapest: store the
  extracted `bool` in your own shadow and compare `bool`s. **Do not memcmp the storage byte.**
* `FEnumProperty` / `FByteProperty`-with-enum — compare the underlying integer
  (`GetUnderlyingProperty()->GetSignedIntPropertyValue`). Cheapest possible.
* `FObjectProperty` and friends — compare the raw pointer / `FWeakObjectPtr` bits.
  `FObjectProperty::Identical` is overridden (`UnrealType.h:3185`) and can do a deep
  `StaticIdentical` (`UnrealType.h:2877`) under some `PortFlags`; a pointer compare is both cheaper
  and what a UI binding wants.
* `FTextProperty` — **`Identical` is unreliable as a change signal in a cooked build:**
  ```cpp
  // Runtime/CoreUObject/Private/UObject/TextProperty.cpp:63-67
  bool FTextProperty::Identical_Implementation(const FText& ValueA, const FText& ValueB, uint32 PortFlags)
  {
      // We compare the display strings in editor (as we author in the native language)
      return Identical_Implementation(ValueA, ValueB, PortFlags, GIsEditor ? EIdenticalLexicalCompareMethod::DisplayString : EIdenticalLexicalCompareMethod::None);
  }
  ```
  With `EIdenticalLexicalCompareMethod::None` and non-invariant, non-transient text, the function
  falls through to *"If we got this far then the texts don't share the same pointer, which means that
  they can't share the same identity"* → `return false` (`TextProperty.cpp:119-120`). Two `FText`s
  with identical display strings but distinct identities compare **not equal** in a game build. For
  UI, shadow `FTextInspector::GetDisplayString(...)` (or `FText::ToString()`) and compare strings.
* `FStructProperty` — `Identical` delegates to `UScriptStruct::CompareScriptStruct`:
  ```cpp
  // Runtime/CoreUObject/Private/UObject/Class.cpp:3663-3694 (excerpt)
      if (StructFlags & STRUCT_IdenticalNative)
      {
          UScriptStruct::ICppStructOps* TheCppStructOps = GetCppStructOps();
          ...
          if (TheCppStructOps->Identical(A, B, PortFlags, bResult)) { return bResult; }
      }

      for( TFieldIterator<FProperty> It(this); It; ++It )
      {
          for( int32 i=0; i<It->ArrayDim; i++ )
          {
              if( !It->Identical_InContainer(A,B,i,PortFlags) ) { return false; }
          }
      }
  ```
  i.e. a native `Identical` if the struct declares one, otherwise a **fresh `TFieldIterator` walk per
  call**. Cheapest correct: flatten the struct once at build time into leaf commands (RepLayout
  style) and diff leaves; or, for POD structs (`STRUCT_IsPlainOldData`), `memcmp` the whole thing.
* `FArrayProperty` — O(n) element-wise (`PropertyArray.cpp:89-116`), with a `Num()` early-out.
  Acceptable.
* `FMapProperty` / `FSetProperty` — **O(n²) worst case.** `Identical` calls `IsPermutation`
  (`PropertyMap.cpp:85-148`, `PropertySet.cpp:190-210`), which after skipping a common prefix does
  `AnyEqual(...) && RangesContainSameAmountsOfVal(...)` per remaining element
  (`PropertyMap.cpp:132-147`). Never call this per frame on a large map. Cheapest correct-enough:
  diff `Num()` plus a per-entry hash, or make map/set bindings explicitly push-based.

---

## 5. Metadata — **not available at runtime**

### 5.1 The gate

The entire `FField` metadata API is inside one `#if WITH_METADATA` block:

```
Runtime/CoreUObject/Public/UObject/Field.h:916   #if WITH_METADATA
Runtime/CoreUObject/Public/UObject/Field.h:919       TMap<FName, FString>* MetaDataMap;
Runtime/CoreUObject/Public/UObject/Field.h:928       bool HasMetaData(const TCHAR* Key) const { return FindMetaData(Key) != nullptr; }
Runtime/CoreUObject/Public/UObject/Field.h:937       COREUOBJECT_API const FString* FindMetaData(const TCHAR* Key) const;
Runtime/CoreUObject/Public/UObject/Field.h:946       COREUOBJECT_API const FString& GetMetaData(const TCHAR* Key) const;
Runtime/CoreUObject/Public/UObject/Field.h:1066      COREUOBJECT_API const TMap<FName, FString>* GetMetaDataMap() const;
Runtime/CoreUObject/Public/UObject/Field.h:1073  #endif // WITH_METADATA
```

`UStruct::GetBoolMetaDataHierarchical` / `GetStringMetaDataHierarchical` / `HasMetaDataHierarchical`
are likewise `#if WITH_METADATA` (`Class.h:874-888`). `FMetaData` itself (`MetaData.h:66-278`) and
`UPackage::GetMetaData()` (`Package.h:1144-1152`) are the same.

And:

```cpp
// Runtime/Core/Public/Misc/CoreMiscDefines.h:29-34
/** This controls if metadata for compiled in classes is unpacked and setup at boot time. Meta data is not normally used except by the editor. **/
#ifndef WITH_METADATA
#define WITH_METADATA WITH_EDITORONLY_DATA
#elif WITH_EDITORONLY_DATA && !WITH_METADATA
#error WITH_EDITORONLY_DATA=1 requires WITH_METADATA=1
#endif
```

```csharp
// Programs/UnrealBuildTool/Configuration/Rules/TargetRules.cs:1201-1204
        public bool bBuildWithEditorOnlyData
        {
            get => bBuildWithEditorOnlyDataOverride ?? (Type == TargetType.Editor || Type == TargetType.Program);
```

```csharp
// Programs/UnrealBuildTool/Configuration/UEBuildTarget.cs:6554-6556
            if (Rules.bBuildWithEditorOnlyData == false)
            {
                GlobalCompileEnvironment.Definitions.Add("WITH_EDITORONLY_DATA=0");
```

⇒ For `TargetType.Game` / `Client` / `Server`, `WITH_EDITORONLY_DATA=0` ⇒ `WITH_METADATA=0` ⇒
**`HasMetaData`/`GetMetaData` do not compile.** Not "return empty" — the member functions are not
declared. Code that calls them fails to build for a packaged target.

It goes further than the API: UHT does not even *emit* the data. Metadata literal arrays in
generated code are wrapped:

```
Engine/Intermediate/Build/Linux/UnrealEditor/Inc/MovieSceneTools/UHT/MovieSceneToolsUserSettings.gen.cpp:105
    #if WITH_METADATA
        static constexpr UECodeGen_Private::FMetaDataPairParam Type_MetaData[] = { ... };
    ...
:140
    #endif // WITH_METADATA
```

and the parameter that carries them expands to nothing:

```cpp
// Runtime/CoreUObject/Public/UObject/UObjectGlobals.h:4075-4087
// METADATA_PARAMS(x, y) expands to x, y, if WITH_METADATA is set, otherwise expands to nothing
#if WITH_METADATA
    #define METADATA_PARAMS(x, y) x, y,
#else
    #define METADATA_PARAMS(x, y)
#endif
...
#if WITH_METADATA
    #define IF_WITH_METADATA(...) __VA_ARGS__
#else
    #define IF_WITH_METADATA(...)
#endif
```

The engine states the consequence in its own doc comment:

```cpp
// Runtime/CoreUObject/Public/UObject/Class.h:3013-3021
    /**
     * Finds the localized display name or native display name as a fallback.
     * If called from a cooked build this will normally return the short name as Metadata is not available.
```

### 5.2 Cooked metadata is not an escape hatch

`UStructCookedMetaData` / `UClassCookedMetaData` (`Runtime/CoreUObject/Public/UObject/CookedMetaData.h:153-191`)
look like they preserve metadata into cooked packages. They do not help:

* Both `CacheMetaData` and `ApplyMetaData` bodies are `#if WITH_METADATA`
  (`Runtime/CoreUObject/Private/UObject/CookedMetaData.cpp:83-92`, `97-103`, `117-120`, `125-128`),
  as is the loader hook `PreloadCookedMetaData` (`CookedMetaData.cpp:29-39`). With
  `WITH_METADATA=0` they are no-ops.
* They are only *written* under `SAVE_Optional` during cook:
  ```cpp
  // Runtime/Engine/Private/BlueprintGeneratedClass.cpp:2282-2291
      if (ObjectSaveContext.IsCooking() && (ObjectSaveContext.GetSaveFlags() & SAVE_Optional))
      {
          UClassCookedMetaData* CookedMetaData = NewCookedMetaData();
          CookedMetaData->CacheMetaData(this);
  ```
  — i.e. editor-optional-data chunks, not the game package.

**[inference]** Forcing `GlobalDefinitions.Add("WITH_METADATA=1")` in a Game `.Target.cs` is *legal*
per `CoreMiscDefines.h:32-34` (the `#error` only fires the other way round) and would restore both
the API and the UHT-emitted native metadata. It would **not** restore metadata for Blueprint-authored
types, whose metadata lives in `UPackage`'s `FMetaData` and is stripped at cook. It would also grow
the shipping binary by every tooltip and comment string. Not recommended.
**Experiment to settle it:** add `WITH_METADATA=1` to a Game target, cook, and at runtime print
`FProperty::GetMetaDataMap()` for (a) a native `USTRUCT` and (b) a `UUserDefinedStruct`. Expect (a)
populated, (b) empty.

### 5.3 Runtime-safe alternatives for "which properties are exposed"

Ranked by robustness:

1. **`EPropertyFlags`.** These are compiled into `FPropertyParams` unconditionally (see the
   `(EPropertyFlags)0x0010000000004001` literal in the generated file quoted above) and are readable
   via `GetPropertyFlags()` / `HasAnyPropertyFlags` / `HasAllPropertyFlags`
   (`UnrealType.h:1269, 1288, 1299`). Useful bits (`ObjectMacros.h`):
   `CPF_Edit` 434, `CPF_BlueprintVisible` 436, `CPF_BlueprintReadOnly` 438, `CPF_Transient` 447,
   `CPF_SaveGame` 458, `CPF_Deprecated` 463, `CPF_Interp` 467, `CPF_EditorOnly` 469,
   `CPF_AssetRegistrySearchable` 474, `CPF_ExposeOnSpawn` 482.
   A sane default policy: expose iff `HasAnyPropertyFlags(CPF_BlueprintVisible)` and
   `!HasAnyPropertyFlags(CPF_Deprecated | CPF_EditorOnly)`.
   `FProperty::IsEditorOnlyProperty()` (`UnrealType.h:1320-1323`) is the shorthand for `CPF_DevelopmentAssets`
   (== `CPF_EditorOnly`, `ObjectMacros.h:519`).
2. **A repurposed flag as an opt-in marker.** `UPROPERTY(SaveGame)` → `CPF_SaveGame`, or
   `UPROPERTY(Interp)` → `CPF_Interp`, both survive cooking and are cheap bit tests. Ugly but
   zero-infrastructure.
3. **An explicit, reflected descriptor.** A `USTRUCT`/`UDataAsset` listing `FName`s per exposed type,
   authored in the editor and cooked as normal asset data. This is the only approach that can carry
   arbitrary per-property *attributes* (format string, unit, clamp) into a shipped build. Recommended
   if VaCuus needs anything richer than a boolean "exposed".
4. **Naming convention** on `FProperty::GetFName()` — works, but brittle.

Names that *are* available at runtime: `FField::GetFName()` / `GetName()`, and
`FField::GetAuthoredName()` (`Field.h:899-903`, outside all metadata guards; see §7.4).

---

## 6. Thread safety

### 6.1 Type descriptors (`UStruct`, `UScriptStruct`, `UClass`, `FProperty`)

**Mutation points**, all found:

* `UStruct::Link(FArchive&, bool bRelinkExistingProperties)`
  (`Runtime/CoreUObject/Private/UObject/Class.cpp:854`) writes `PropertiesSize`, `MinAlignment`, every
  `FProperty::Offset_Internal` (via `Property->Link(Ar)`, `Class.cpp:912`), and rebuilds
  `PropertyLink`/`RefLink`/`DestructorLink`/`PostConstructLink` (`Class.cpp:1036-1102`). Runs at
  compiled-in type construction (`UObjectConstructInternal.h:203` `NewStruct->StaticLink();`), at
  package load, and on Blueprint recompile.
* `UStruct::AddCppProperty` (`Class.cpp:723-727`) prepends to `ChildProperties`.
* `UPropertyBag::GetOrCreateFromDescs` creates whole new `UScriptStruct`s **at runtime, from any
  thread**, and takes an explicit lock to do it:
  ```cpp
  // Runtime/CoreUObject/Private/StructUtils/PropertyBag.cpp:4157-4159
      // We need to linearize this entire function otherwise threads that create bags of identical layouts can view
      // partially-constructed objects
      UE::TScopeLock ScopeLock(UE::StructUtils::Private::GPropertyBagLock);
  ```
* Lazy per-struct caches that *are* atomic:
  ```cpp
  // Runtime/CoreUObject/Public/UObject/Class.h:576-577
      /** Cached schema for optimized unversioned and filtereditoronly property serialization, owned by this. */
      mutable std::atomic<const struct FUnversionedStructSchema*> UnversionedGameSchema = nullptr;
  ```
* Editor-only mutation: `FProperty::GetUPropertyWrapper()` (`UnrealType.h:344-347`),
  `FField::SetMetaData` (`Field.h:966-970`), `UStruct::PropertyWrappers` (`Class.h:565-570`).

**Conclusion.** For a type that is already fully loaded and linked, nothing in a cooked runtime
rewrites its `ChildProperties`/`PropertyLink` chain or its `FProperty` offsets/flags. Reading the
descriptor from a worker thread is safe. **[inference]** — there is no engine comment guaranteeing
it, but it is corroborated by `PropertyBag.cpp:4157-4159` (which locks only *creation*, then hands
the finished `UPropertyBag*` to arbitrary threads) and by the async loading thread constructing
types concurrently with game-thread reflection use.

**Caveats:**

* A type being loaded *right now* by the async loading thread is being `Link()`ed concurrently. Only
  walk types you obtained from something already resolved (`FSomeStruct::StaticStruct()`, an
  already-loaded asset's `GetClass()`).
* Do not touch metadata, `GetUPropertyWrapper`, or anything `WITH_EDITOR` from a worker.
* `UStruct::GetCppStructOps()` asserts if `PrepareCppStructOps()` has not run:
  ```cpp
  // Runtime/CoreUObject/Public/UObject/Class.h:2367-2371
      inline ICppStructOps* GetCppStructOps() const
      {
          checkf(bPrepareCppStructOpsCompleted, TEXT("GetCppStructOps: PrepareCppStructOps() has not been called for class %s"), *GetPathName());
          return CppStructOps;
      }
  ```

### 6.2 GC and holding descriptor pointers

Native `UClass`es are created root-set:

```cpp
// Runtime/CoreUObject/Private/UObject/UObjectConstructInternal.h:114
            EObjectFlags(RF_Public | RF_Standalone | RF_Transient | RF_MarkAsNative | RF_MarkAsRootSet),
```

```cpp
// Runtime/CoreUObject/Private/UObject/UObjectBase.cpp:259-266
    if (ObjectFlags & RF_MarkAsRootSet)
    {
        InternalFlagsToSet |= EInternalObjectFlags::RootSet;
        ObjectFlags &= ~RF_MarkAsRootSet;
    }
    if (ObjectFlags & RF_MarkAsNative)
    {
        InternalFlagsToSet |= EInternalObjectFlags::Native;
```

and `Native` is itself a keep-flag:

```cpp
// Runtime/CoreUObject/Public/UObject/ObjectMacros.h:707-709
#define EInternalObjectFlags_GarbageCollectionKeepFlags (EInternalObjectFlags::Borrowed | EInternalObjectFlags::Native | EInternalObjectFlags::Async | EInternalObjectFlags::AsyncLoadingPhase1 | EInternalObjectFlags::AsyncLoadingPhase2 | EInternalObjectFlags::LoaderImport)
...
#define EInternalObjectFlags_RootFlags (EInternalObjectFlags::RootSet | EInternalObjectFlags_GarbageCollectionKeepFlags)
```

Native `UScriptStruct`s carry `RF_MarkAsNative` too (generated `.gen.cpp` `FStructParams` shows
`RF_Public|RF_Transient|RF_MarkAsNative`, e.g.
`Engine/Intermediate/Build/Linux/UnrealEditor/Inc/PortalMessages/UHT/PortalApplicationWindowMessages.gen.cpp:72`).

⇒ **Raw `UScriptStruct*` / `UClass*` for native types are safe to hold indefinitely from any thread.**
`UUserDefinedStruct` and `UBlueprintGeneratedClass` are **not** native and can be collected — hold
those with `TStrongObjectPtr` or keep the owning asset alive.

To block GC from a worker while touching UObjects:

```cpp
// Runtime/CoreUObject/Public/UObject/GarbageCollection.h:116-122
/** Prevent GC from running in the current scope */
class FGCScopeGuard
{
public:
    COREUOBJECT_API FGCScopeGuard();
    COREUOBJECT_API ~FGCScopeGuard();
};
```

```cpp
// Runtime/CoreUObject/Private/UObject/GarbageCollection.cpp:191-196
FGCScopeGuard::FGCScopeGuard()
{
    ...
    FGCCSyncObject::Get().LockAsync();
```

```cpp
// Runtime/CoreUObject/Private/UObject/GCScopeLock.h:51-56
    /** Lock on non-game thread. Will block if GC is running. */
    void LockAsync()
    {
        if (!IsInGameThread())
        {
            // Wait until GC is done if it was running when entering this function
```

Note the guard is a **no-op on the game thread** and can **block for the full GC duration** on a
worker — `GarbageCollection.cpp:199-202` warns above 1 ms, with the comment *"Note this is expected
to take roughly the time it takes to collect garbage and verify GC assumptions, so up to 300ms in
development"* (`GarbageCollection.cpp:201`). Do not hold it
across a frame's worth of work. `FGCScopeTryGuard` (`GarbageCollection.h:124-134`) is the
non-blocking variant. `IsGarbageCollecting()` (`GarbageCollection.h:204-207`) reads
`TSAN_ATOMIC(bool) GIsGarbageCollecting`.

The canonical engine use is `FWeakObjectPtr::Internal_Pin`
(`Runtime/CoreUObject/Private/UObject/WeakObjectPtr.cpp:161-166`) — take the guard, resolve, hand
back a `TStrongObjectPtr`.

### 6.3 Instance data

**No engine guarantee whatsoever.** Reading a `UObject`'s properties while the game thread may write
them is a plain data race: `ContainerPtrToValuePtr` + dereference is an unsynchronised load, and for
`FString`/`TArray`/`TMap` it dereferences a pointer the game thread may be reallocating. There is no
per-object lock, no seqlock, no `volatile`.

### 6.4 What this means for VaCuus

| Stage | Thread | Justification |
|---|---|---|
| Build the property description from `UScriptStruct`/`UClass` | **any thread** (for already-loaded types) | §6.1, §6.2 |
| Read instance values into the snapshot | **game thread only** | §6.3 |
| Diff snapshot vs. previous snapshot (both VaCuus-owned buffers) | any thread | your memory, your rules |
| Serialise / ship the diff to the UI thread | any thread | ditto |

So the intended split works: **builder anywhere, sampler on the game thread, everything downstream
anywhere** — provided the snapshot is a *value copy* that owns its strings and does not retain raw
`UObject*` (store `FWeakObjectPtr`, or resolve to a name/path at sample time).

---

## 7. Blueprint exposure

### 7.1 Reaching Blueprint

* `USTRUCT(BlueprintType)` — `ObjectMacros.h:1216-1231`:
  ```cpp
  namespace US
  {
      // valid keywords for the USTRUCT macro
      enum
      {
          ...
          /// Exposes this struct as a type that can be used for variables in blueprints
          BlueprintType,
  ```
* Per-member: `UPROPERTY(BlueprintReadWrite)` / `BlueprintReadOnly` → `CPF_BlueprintVisible`
  (`ObjectMacros.h:436`) and `CPF_BlueprintReadOnly` (`:438`).
* `UCLASS(BlueprintType)` — `ObjectMacros.h:844`.

### 7.2 A "create model from struct" node — the wildcard-struct shape

The pattern is `CustomThunk` + `meta=(CustomStructureParam=…)` with an `int32&` placeholder in the
signature, plus a manual `DECLARE_FUNCTION`/`DEFINE_FUNCTION`. Specifiers:
`UF::CustomThunk` `ObjectMacros.h:1050`, `UM::CustomStructureParam` `ObjectMacros.h:1694`
(*"[FunctionMetadata] Used with CustomThunk to declare that a parameter is actually polymorphic"*).

Declaration (`Runtime/Engine/Classes/Kismet/BlueprintInstancedStructLibrary.h:28-29, 80`):

```cpp
    UFUNCTION(BlueprintCallable, CustomThunk, Category = "Utilities|Instanced Struct", meta = (CustomStructureParam = "Value", BlueprintInternalUseOnly="true", NativeMakeFunc))
    static ENGINE_API FInstancedStruct MakeInstancedStruct(const int32& Value);
    ...
    DECLARE_FUNCTION(execMakeInstancedStruct);
```

Stub + thunk (`Runtime/Engine/Private/BlueprintInstancedStructLibrary.cpp:12-17, 31-62`):

```cpp
FInstancedStruct UBlueprintInstancedStructLibrary::MakeInstancedStruct(const int32& Value)
{
    // We should never hit this! stubs to avoid NoExport on the class.
    checkNoEntry();
    return {};
}

DEFINE_FUNCTION(UBlueprintInstancedStructLibrary::execMakeInstancedStruct)
{
    // Read wildcard Value input.
    Stack.MostRecentPropertyAddress = nullptr;
    Stack.MostRecentPropertyContainer = nullptr;
    Stack.StepCompiledIn<FStructProperty>(nullptr);

    const FStructProperty* ValueProp = CastField<FStructProperty>(Stack.MostRecentProperty);
    const void* ValuePtr = Stack.MostRecentPropertyAddress;

    P_FINISH;

    if (!ValueProp || !ValuePtr)
    {
        FBlueprintExceptionInfo ExceptionInfo(
            EBlueprintExceptionType::AbortExecution,
            LOCTEXT("InstancedStruct_MakeInvalidValueWarning", "Failed to resolve the Value for Make Instanced Struct")
        );

        FBlueprintCoreDelegates::ThrowScriptException(P_THIS, Stack, ExceptionInfo);
        ...
    }
    else
    {
        P_NATIVE_BEGIN;
        (*(FInstancedStruct*)RESULT_PARAM).InitializeAs(ValueProp->Struct, (const uint8*)ValuePtr);
        P_NATIVE_END;
    }
}
```

That gives you both the `UScriptStruct*` (`ValueProp->Struct`) and the instance pointer
(`Stack.MostRecentPropertyAddress`) in one node. `FFrame::MostRecentProperty` /
`MostRecentPropertyAddress` are declared at
`Runtime/CoreUObject/Public/UObject/Stack.h:122-123`; the "did the step resolve?" convenience is
`FFrame::StepAndCheckMostRecentProperty` (`Stack.h:194-195`, impl `:462-467`).

Copy the null-check + `ThrowScriptException` shape exactly — without it, a disconnected or
mismatched pin gives you a null `ValuePtr` and a crash instead of a Blueprint runtime error.

For an object-driven model, `UFUNCTION(BlueprintCallable)` taking `UObject*` needs no thunk; get the
type via `Obj->GetClass()`.

### 7.3 Blueprint structs vs. C++ structs at the reflection level

| | C++ `USTRUCT` | Blueprint struct |
|---|---|---|
| Type object | `UScriptStruct` | `UUserDefinedStruct : UScriptStruct` (`Runtime/CoreUObject/Public/StructUtils/UserDefinedStruct.h:71`) |
| `GetCppStructOps()` | native ops (compare/copy/serialize) | **always `nullptr`** — `UUserDefinedStruct::PrepareCppStructOps` sets `CppStructOps = nullptr` with the comment *"User structs can never have struct ops…"* (`Runtime/CoreUObject/Private/StructUtils/UserDefinedStruct.cpp:58-63`) |
| `Identical` path | may short-circuit via `STRUCT_IdenticalNative` | always the per-property `TFieldIterator` loop (`Class.cpp:3672-3693`) |
| GC | native → root set (§6.2) | ordinary UObject, collectable |
| Property names | as written | **mangled — see §7.4** |
| Default values | native constructor | a stored default instance, `GetDefaultInstance()` (`UserDefinedStruct.h:149`) |
| Stable ID | none | `Guid` (`UserDefinedStruct.h:93-94`) + per-property `PropertyIDs` (`:97-98`) |

### 7.4 The name mangling — exact format

Blueprint struct members are named `<Base>_<UniqueId>_<Guid32Hex>`:

```cpp
// Editor/UnrealEd/Private/Kismet2/StructureEditorUtils.cpp:256-264
        const uint32 UniqueNameId = CastChecked<UUserDefinedStructEditorData>(Struct->EditorData)->GenerateUniqueNameIdForMemberVariable();
        const FString FriendlyName = FString::Printf(TEXT("%s_%u"), *Result, UniqueNameId);
        if (OutFriendlyName)
        {
            *OutFriendlyName = FriendlyName;
        }
        const FName NameResult = *FString::Printf(TEXT("%s_%s"), *FriendlyName, *Guid.ToString(EGuidFormats::Digits));
        check(NameResult.IsValidXName(INVALID_OBJECTNAME_CHARACTERS));
        return NameResult;
```

A variable the user calls `Health` becomes something like
`Health_2_9F3C1A084F6E4B2D8E7A5C0B1D2E3F40` — `FProperty::GetName()` returns *that*. The base name is
`"MemberVar"` when the user did not name it (`StructureEditorUtils.cpp:251-254`).

The reverse:

```cpp
// Editor/UnrealEd/Private/Kismet2/StructureEditorUtils.cpp:267-285
    static FGuid GetGuidFromName(const FName Name)
    {
        const FString NameStr = Name.ToString();
        const int32 GuidStrLen = 32;
        if (NameStr.Len() > (GuidStrLen + 1))
        {
            const int32 UnderscoreIndex = NameStr.Len() - GuidStrLen - 1;
            if (TCHAR('_') == NameStr[UnderscoreIndex])
            {
                const FString GuidStr = NameStr.Right(GuidStrLen);
```

Exposed as `FStructureEditorUtils::GetGuidFromPropertyName(FName)`
(`Editor/UnrealEd/Public/Kismet2/StructureEditorUtils.h:167`) — **editor module, do not link from
runtime code.**

The editor-side authored record is `FStructVariableDescription`
(`Editor/UnrealEd/Classes/UserDefinedStructure/UserDefinedStructEditorData.h:34-124`):

```cpp
    UPROPERTY()
    FName VarName;        // the mangled name

    UPROPERTY()
    FGuid VarGuid;        // the stable identity

    UPROPERTY()
    FString FriendlyName; // "<Base>_<UniqueId>" — still not the raw display label

    ...
    UPROPERTY()
    TMap<FName, FString> MetaData;
```

**So there are three names, and they are all different:**

* **Authored / mangled name** — `FProperty::GetName()` / `GetFName()`. `Health_2_9F3C…`. Stable
  within one compile of the struct; the GUID part is stable across recompiles, the `_2` counter is
  not necessarily.
* **Display name** — what the user typed, `Health`. In the editor it comes from
  `UUserDefinedStructEditorDataBase::GetFriendlyNameForProperty`.
* **Authored name** — `FField::GetAuthoredName()` (`Field.h:903`), which delegates to
  `UStruct::GetAuthoredNameForField` (`Class.h:863`, default impl `Class.cpp:2558-2565` = just
  `GetName()`), overridden by `UUserDefinedStruct`:
  ```cpp
  // Runtime/CoreUObject/Private/StructUtils/UserDefinedStruct.cpp:281-314 (excerpt)
  FString UUserDefinedStruct::GetAuthoredNameForField(const FField* Field) const
  {
      ...
  #if WITH_EDITOR
      if (const UUserDefinedStructEditorDataBase* StructEditorData = Cast<const UUserDefinedStructEditorDataBase>(EditorData))
      {
          const FString EditorName = StructEditorData->GetFriendlyNameForProperty(this, Property);
          if (!EditorName.IsEmpty()) { return EditorName; }
      }
  #endif  // WITH_EDITOR

      const int32 GuidStrLen = 32;
      const int32 MinimalPostfixlen = GuidStrLen + 3;
      const FString OriginalName = Property->GetName();
      if (OriginalName.Len() > MinimalPostfixlen)
      {
          FString DisplayName = OriginalName.LeftChop(GuidStrLen + 1);
          int FirstCharToRemove = -1;
          const bool bCharFound = DisplayName.FindLastChar(TCHAR('_'), FirstCharToRemove);
          if (bCharFound && (FirstCharToRemove > 0))
          {
              return DisplayName.Mid(0, FirstCharToRemove);
          }
      }
      return OriginalName;
  }
  ```
  The `#if WITH_EDITOR` block is only a *preference*; the string-chopping fallback below it runs in a
  packaged game. `Field.h:899-902` documents the contract: *"This name is consistent in
  editor/cooked builds, is not localized, and is useful for data import/export."*

⇒ **`GetAuthoredName()` is the right thing to put on the wire for a property label.** It gives
`Health` for a Blueprint struct and `Health` for a C++ struct, in both editor and cooked builds, with
no metadata dependency. (It returns an `FString` by value — call it once at build time, not per
frame.)

### 7.5 Stable IDs at runtime

`UStruct` exposes a GUID mapping that is *not* editor-only:

```cpp
// Runtime/CoreUObject/Public/UObject/Class.h:989-995
    virtual FName FindPropertyNameFromGuid(const FGuid& PropertyGuid) const { return NAME_None; }
    ...
    virtual FGuid FindPropertyGuidFromName(const FName InName) const { return FGuid(); }
    ...
    virtual bool ArePropertyGuidsAvailable() const { return false; }
```

* `UUserDefinedStruct` implements them off a runtime `UPROPERTY` array
  (`UserDefinedStruct.h:96-98` `TArray<UE::StructUtils::FUserDefinedPropertyID> PropertyIDs;`,
  impl `UserDefinedStruct.cpp:65-87`), and `ArePropertyGuidsAvailable()` is
  `PropertyIDs.Num() > 0` (`UserDefinedStruct.h:140-143`). Available in cooked builds.
* `UBlueprintGeneratedClass` implements them off `CookedPropertyGuids`
  (`Runtime/Engine/Classes/Engine/BlueprintGeneratedClass.h:526-528`, a non-editor `UPROPERTY`) —
  but **only if the cooker was told to emit them**:
  ```cpp
  // Runtime/Engine/Private/BlueprintGeneratedClass.cpp:2259-2261
      if (ObjectSaveContext.IsCooking() && ShouldCookBlueprintPropertyGuids())
      {
          CookedPropertyGuids = PropertyGuids;
  ```
  gated on `UCookerSettings::BlueprintPropertyGuidsCookingMethod`
  (`EnabledBlueprintsOnly` / `AllBlueprints` / `Disabled`, `BlueprintGeneratedClass.cpp:2240-2256`),
  and `ArePropertyGuidsAvailable()` returns false in a non-editor build when they were not cooked
  (`BlueprintGeneratedClass.cpp:2915-2929`).

⇒ Guard any GUID-keyed design with `ArePropertyGuidsAvailable()` and fall back to
`GetAuthoredName()`.

### 7.6 Blueprint enums

`UUserDefinedEnum::DisplayNameMap` is a plain `UPROPERTY()`, **not** `WITH_EDITORONLY_DATA`
(`Runtime/Engine/Classes/Engine/UserDefinedEnum.h:41-42`), and
`GetDisplayNameTextByIndex` / `GetAuthoredNameStringByIndex` are overridden to read it
(`UserDefinedEnum.h:66-68`). So BP enum labels *are* available at runtime. Native enum
`DisplayName` metadata is **not** (§5, and `Class.h:3013-3016` says so explicitly) — for native
enums use `UEnum::GetNameStringByValue` (`Class.h:2996`) or
`GetAuthoredNameStringByValue` (`Class.h:3041`).

---

## 8. Traps and 5.x deprecations

**Renamed / removed (would look like a compile error, but check what you replace it with):**

| Old | 5.8 replacement | Evidence |
|---|---|---|
| `FProperty::ExportTextItem(Str, Value, Default, Parent, Flags, Scope)` | `ExportTextItem_Direct` / `ExportTextItem_InContainer` | `UnrealType.h:587, 592`; the protected virtual is `ExportText_Internal` (`:720`) with an `EPropertyPointerType` |
| `FProperty::ImportText(...)` | `ImportText_Direct` / `ImportText_InContainer` | `UnrealType.h:625, 606` |
| `FProperty::ElementSize` (member) | `GetElementSize()` / `SetElementSize()` | `UE_DEPRECATED(5.5, …)` at `UnrealType.h:179-180` |
| `FProperty::Visit(FPropertyVisitorData, …)` | `Visit(FPropertyVisitorContext&, …)` | `UE_DEPRECATED(5.7, …)` at `UnrealType.h:383-384` |
| `FField::SetFlags/GetFlags/HasAnyFlags/HasAllFlags/SetFlagsTo` | `FProperty::PropertyFlags` API | `UE_DEPRECATED(5.8, …)` at `Field.h:736, 739, 741, 743, 745, 747` |
| `FField::PostLoad` / `BeginDestroy` / `Bind` | none — *"It is never called."* | `Field.h:687, 689, 708` |
| `F*Property(FFieldVariant, FName, EObjectFlags)` ctors | drop the `EObjectFlags` argument | `UE_DEPRECATED(5.8, …)` on every property class, e.g. `UnrealType.h:245-246, 2613-2614, 2783-2784`; `EnumProperty.h:35-36` |
| `FField::Duplicate(…, FlagMask, InternalFlagsMask)` | drop those parameters | `Field.h:1112` |
| `FObjectPropertyBase::GetCPPTypeCustom` | `GetCPPType` directly | `UE_DEPRECATED(5.7, …)` at `UnrealType.h:2837` |
| `UEnum::SetEnums` without underlying type | the overload with the underlying type | `UE_DEPRECATED(5.8, …)` at `Class.h:3168` |
| `UClass::ClassDefaultObject` (public member) | `GetDefault<>()` / `GetMutableDefault<>()` | `UE_DEPRECATED(5.6, …)` at `Class.h:4045` |

**Signature churn to watch (silently different, not deprecated):** `Identical`,
`SerializeItem`, `NetSerializeItem` and friends now take `TNotNull<const void*>` / `TNotNull<void*>`
for the primary pointer (`UnrealType.h:517, 583, 584`). Overriding them with the old `const void*`
signature does not override anything — it adds an overload, and the base pure-virtual keeps the
class abstract or the base implementation keeps getting called.

**Silent failures:**

1. `ExportText_Direct`/`ExportText_InContainer` return `false` and write nothing when the value is
   default. §3.3.
2. `Identical(A, nullptr)` means "equals default", not "always true"/"always false". `UnrealType.h:1767-1774`.
3. `EFieldIterationFlags::Default` includes deprecated properties. `UnrealType.h:7143`.
4. Reading a bitfield `bool` as a `bool*`. §2.3.
5. `memcmp` on a scratch buffer that a bitfield `FBoolProperty` wrote into — uninitialised
   neighbouring bits. §2.3 item 3.
6. `FTextProperty::Identical` reports "changed" for equal-looking text in a cooked build. §4.4.
7. `CastField<FStrProperty>` misses `FUtf8StrProperty` / `FAnsiStrProperty` (separate cast flags,
   `ObjectMacros.h:407-408`). New in the 5.x line; a `UPROPERTY(FUtf8String)` will just not appear
   in your model.
8. `CastField<FObjectProperty>` also matches `FClassProperty` (`UnrealType.h:3465`), and
   `CastField<FNumericProperty>` matches `FByteProperty` even when it is really an enum
   (`FNumericProperty::IsEnum()` / `GetIntPropertyEnum()`, `UnrealType.h:1843-1849`).
9. `FScriptMapHelper` / `FScriptSetHelper`: `Num() != GetMaxIndex()` after removals, and
   `GetKeyPtr`/`GetPairPtr` `checkf` on an invalid internal index (`UnrealType.h:4871-4875`). Use
   `CreateIterator()`.
10. Metadata calls that compile fine in the editor target and break the Game target build. §5.
11. `PostEditChangeProperty` overrides silently never firing in a packaged build. §4.3.
12. `FSoftObjectProperty::LoadObjectPropertyValue` (`UnrealType.h:3435`) does a **synchronous load** —
    a per-frame binding that reads a soft reference through the "obvious" `LoadObjectPropertyValue_InContainer`
    (`UnrealType.h:2883-2886`) will hitch. Use `GetObjectPropertyValue` and treat null as
    "not loaded".

**Naming that is itself a warning:** `FProperty` exposes five identical offset accessors —
`GetOffset_ForDebug`, `GetOffset_ForUFunction`, `GetOffset_ForGC`, `GetOffset_ForInternal`, and
`GetOffset_ReplaceWith_ContainerPtrToValuePtr` (`UnrealType.h:446-469`) — all returning
`Offset_Internal`. The last one is named to tell you to stop using it. Use
`ContainerPtrToValuePtr`.

---

## 9. Open questions / experiments

1. **Cost of `TFieldIterator` vs. `PropertyLink` in practice.** Both are linked-list walks; the
   iterator adds a virtual-free cast-flag test and struct-chain hop per node. Not measured here.
   *Experiment:* the builder runs once per type, so this almost certainly does not matter — measure
   only if profiling says so.
2. **Whether `WITH_METADATA=1` in a Game target actually links.** §5.2. Cheap to test, and the
   answer decides whether metadata-driven exposure is even on the table.
3. **Whether a worker-thread descriptor walk ever races a Blueprint hot-reload in PIE.** In-editor
   PIE *does* relink `UBlueprintGeneratedClass` (`UStruct::Link` with `bRelinkExistingProperties`,
   `Class.cpp:854-863`, whose comment names blueprint compilation, package reload and Live Coding).
   *Experiment:* run the builder on a worker while recompiling a Blueprint in PIE under TSan/ASan.
   If it trips, gate the builder to the game thread in editor builds only.
4. **Whether `GIsEditor`-dependent `FTextProperty::Identical` also affects PIE.** `GIsEditor` is true
   in PIE, so text diffs behave differently in PIE than in a packaged build (§4.4). Verify before
   trusting a PIE-only test of text bindings.
