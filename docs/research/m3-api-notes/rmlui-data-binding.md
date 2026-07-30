# RmlUi data binding — verified API reference (vendored copy)

**Source of truth for this document:**
`/w/Unreal/VcHost/Plugins/VaCuus/Source/ThirdParty/RmlUi/`
Vendored SHA `0ae381e00d7426762bb5ed897973366358b16642` (`VENDORED_SHA.txt`).

**All `file:line` citations below are relative to that root** and were opened and read while
writing this document. Nothing here comes from RmlUi's website or from memory. Where I am
reasoning rather than reading, the line is prefixed **[inference]**. Where a question cannot be
settled from source, §10 says what experiment settles it.

**Compilation context (matters for several claims):** the RmlUi sources are compiled into the
`VaCuusRml` module via one-line relay `.cpp` files, e.g.
`Source/VaCuusRml/Private/Gen/relay_Core_DataModel.cpp` contains only
`#include "../../../ThirdParty/RmlUi/Source/Core/DataModel.cpp"`. 190 relay files exist;
**none of them are for `Source/Debugger/`**, so the RmlUi debugger (including its Data Models
inspector panel) is *not* compiled into this plugin. `RMLUI_CUSTOM_RTTI=1` is defined
(`Source/VaCuusRml/VaCuusRml.Build.cs:20`).

---

## 1. Model construction and teardown

### 1.1 The three public entry points

```cpp
DataModelConstructor CreateDataModel(const String& name, DataTypeRegister* data_type_register = nullptr,
                                     bool allow_missing_variables = false);
DataModelConstructor GetDataModel(const String& name);
UnorderedMap<String, DataModelConstructor> GetDataModels() const;
bool RemoveDataModel(const String& name);
```
— `Include/RmlUi/Core/Context.h:261`, `:266`, `:269`, `:275`.

`CreateDataModel` (`Source/Core/Context.cpp:1057`):

```cpp
	if (!data_type_register)
	{
		if (!default_data_type_register)
			default_data_type_register = MakeUnique<DataTypeRegister>();
		data_type_register = default_data_type_register.get();
	}

	auto result = data_models.emplace(name, MakeUnique<DataModel>(data_type_register, allow_missing_variables));
	bool inserted = result.second;
	if (inserted)
	{
		DataModel* model = result.first->second.get();
		PluginRegistry::NotifyDataModelCreate(this, name);
		return DataModelConstructor(model);
	}

	Log::Message(Log::LT_ERROR, "Data model name '%s' already exists.", name.c_str());
	return DataModelConstructor();
```

Facts:

- **The model is owned by the Context, not by a document.** `Include/RmlUi/Core/Context.h:383`:
  `UnorderedMap<String, UniquePtr<DataModel>> data_models;`
- Duplicate name → `LT_ERROR` log and an **empty** `DataModelConstructor` (both `model` and
  `type_register` null). `operator bool` on the constructor (`Include/RmlUi/Core/DataModelHandle.h:105`)
  detects this, but nothing else does — calling `Bind` on an empty constructor dereferences
  `type_register` (`DataModelHandle.h:45`) → crash.
- The default `DataTypeRegister` is lazily created **per Context** and lives at
  `Include/RmlUi/Core/Context.h:385` (`UniquePtr<DataTypeRegister> default_data_type_register`).
  All models in one context share it unless you pass your own.
- If you pass your own `DataTypeRegister*`, `DataModel` stores it as a **raw pointer**
  (`Source/Core/DataModel.h:71`) — you must keep it alive longer than the model.
- `allow_missing_variables` is stored on the model (`Source/Core/DataModel.h:72`) and changes three
  behaviours: `ResolveAddress` returns the address instead of failing
  (`Source/Core/DataModel.cpp:267-272`), `GetVariableInto` stops logging failures
  (`:320-322`), and `DirtyVariable`'s "unknown name" assert is skipped (`:328`).

### 1.2 `DataModelHandle` — a bare pointer

```cpp
class RMLUICORE_API DataModelHandle {
public:
	DataModelHandle(DataModel* model = nullptr);

	bool IsVariableDirty(const String& variable_name);
	void DirtyVariable(const String& variable_name);
	void DirtyAllVariables();

	explicit operator bool() { return model; }

private:
	DataModel* model;
};
```
— `Include/RmlUi/Core/DataModelHandle.h:14-26`.

It is a **non-owning raw pointer with no generation counter**. `Source/Core/DataModelHandle.cpp:13-16`:

```cpp
void DataModelHandle::DirtyVariable(const String& variable_name)
{
	model->DirtyVariable(variable_name);
}
```

No null check. A default-constructed handle crashes; a handle held across `RemoveDataModel`
dangles. `Context.h:272` says exactly this: *"@warning Invalidates all handles and constructors
pointing to the data model."*

### 1.3 Lifetime relative to a document

**A document does not own the model, and unloading a document does not destroy it.**

How a document attaches: `Element::SetParent` (`Source/Core/Element.cpp:2202-2219`) is the *only*
place `data-model` is read:

```cpp
		auto it = attributes.find("data-model");
		if (it == attributes.end())
		{
			SetDataModel(parent->data_model);
		}
		else if (Context* context = GetContext())
		{
			String name = it->second.Get<String>();

			if (DataModel* model = context->GetDataModelPtr(name))
			{
				model->AttachModelRootElement(this);
				SetDataModel(model);
			}
			else
				Log::Message(Log::LT_ERROR, "Could not locate data model '%s' in element %s.", ...);
		}
```

`Element::SetDataModel` (`Source/Core/Element.cpp:2145-2166`) propagates down the subtree and, for
each element, calls `ElementUtilities::ApplyDataViewsControllers(this)` (`:2162`), which is where
`data-*` attributes become views/controllers (`Source/Core/ElementUtilities.cpp:384-490`).

Detach path on unload: `Context::UnloadDocument` → `root->RemoveChild(document)`
(`Source/Core/Context.cpp:344`) → `Element::RemoveChild` → `detached_child->SetParent(nullptr)`
(`Source/Core/Element.cpp:1492`) → `SetDataModel(nullptr)` (`:2198-2199`) → for every element in the
subtree, `data_model->OnElementRemove(this)` (`:2157`).

`DataModel::OnElementRemove` (`Source/Core/DataModel.cpp:365-371`):

```cpp
	EraseAliases(element);
	views->OnElementRemove(element);
	controllers->OnElementRemove(element);
	attached_elements.erase(element);
```

**So an unload drops views, controllers, aliases and attachment — and leaves `variables`,
`function_variable_definitions`, `event_callbacks` and `dirty_variables` completely untouched.**

### 1.4 Can a model be created before a document loads, and survive a reload?

- **Created before the document loads: required, not optional.** `Element::SetParent` looks the model
  up by name at attach time and logs `LT_ERROR` if absent (`Source/Core/Element.cpp:2218`). There is
  no retry. Create every model before `LoadDocument`.
- **Survives reload: yes, provided you never call `RemoveDataModel`.** After the reload, the new
  elements re-run `ApplyDataViewsControllers`, the fresh views land in `views_to_add`, and
  `DataViews::Update` unconditionally treats every newly added view as dirty
  (`Source/Core/DataView.cpp:76-88`) — so a reloaded document picks up current values without you
  having to call `DirtyAllVariables`. `Context::LoadDocument` also runs one model update right after
  the `load` event, deliberately *without* clearing dirty flags (`Source/Core/Context.cpp:302-306`).
- **`RemoveDataModel` is a one-way door for already-loaded documents.**
  `Source/Core/Context.cpp:1097-1114` calls `element->SetDataModel(nullptr)` on every attached root.
  Nothing ever re-runs the `data-model` lookup except `Element::SetParent`, and
  `Element::SetDataModel` is **private** (`Include/RmlUi/Core/Element.h:659` `private:`, `:662`) with
  `Rml::Context` as a friend (`:789`). So after `RemoveDataModel` + `CreateDataModel` with the same
  name, a still-loaded document stays permanently detached.
  **[inference]** The escape hatch is to put `data-model="…"` on a child element rather than on
  `<body>`: detaching and re-appending that element re-runs `SetParent` and re-resolves the model.
  I read the code path but did not run it — see §10.
- Because `DataModel::BindVariable` refuses duplicate names (`Source/Core/DataModel.cpp:132-137`) and
  **there is no unbind API at all** (`Source/Core/DataModel.h:25-56` — no remove/erase for variables,
  funcs or event callbacks), re-binding on reload is impossible without destroying the model.

### 1.5 Context destruction

`Context::~Context` (`Source/Core/Context.cpp:105-122`) unloads all documents *first*, then
`data_models.clear()`. That ordering matters: `DataModel::~DataModel` asserts
`attached_elements.empty()` (`Source/Core/DataModel.cpp:102-105`).

### 1.6 Plugin notification hooks

`PluginRegistry::NotifyDataModelCreate/Destroy` fire at `Source/Core/Context.cpp:1071`, `:1109` and
`:114`. `Include/RmlUi/Core/Plugin.h:56-61` documents that during `OnDataModelDestroy` the model is
still reachable via `Context::GetDataModel` but is unusable once the callback returns. This is a
usable hook for a VaCuus-side "model is going away" signal.

---

## 2. The binding surface, exhaustively

All of `DataModelConstructor` (`Include/RmlUi/Core/DataModelHandle.h:28-113`):

| Member | Line | Signature |
|---|---|---|
| `GetModelHandle` | `:37` | `DataModelHandle GetModelHandle() const` |
| `Bind<T>` | `:41-46` | `bool Bind(const String& name, T* ptr)` |
| `BindFunc` | `:49` | `bool BindFunc(const String& name, DataGetFunc get_func, DataSetFunc set_func = {})` |
| `BindEventCallback` | `:52` | `bool BindEventCallback(const String& name, DataEventFunc event_func)` |
| `BindEventCallback<T>` | `:56-61` | `bool BindEventCallback(const String& name, DataEventMemberFunc<T> member_func, T* object_pointer)` |
| `BindCustomDataVariable` | `:65` | `bool BindCustomDataVariable(const String& name, DataVariable data_variable)` |
| `RegisterScalar<T>` | `:72`, impl `:116-133` | `bool RegisterScalar(DataTypeGetFunc<T> get_func, DataTypeSetFunc<T> set_func = {})` |
| `RegisterStruct<T>` | `:78`, impl `:151-167` | `StructHandle<T> RegisterStruct()` |
| `RegisterCustomDataVariableDefinition<T>` | `:82`, impl `:136-148` | `bool RegisterCustomDataVariableDefinition(UniquePtr<VariableDefinition> definition)` |
| `RegisterArray<Container>` | `:91`, impl `:170-189` | `bool RegisterArray()` |
| `RegisterTransformFunc` | `:96-99` | `void RegisterTransformFunc(const String& name, DataTransformFunc transform_func)` |
| `GetDataTypeRegister` | `:103` | `DataTypeRegister* GetDataTypeRegister() const` |
| `operator bool` | `:105` | `explicit operator bool()` |

And `StructHandle<Object>` (`Include/RmlUi/Core/DataStructHandle.h`), returned by `RegisterStruct`:

| Member | Line | Signature |
|---|---|---|
| member object | `:29-32` | `bool RegisterMember(const String& name, MemberType Object::*member_object_ptr)` |
| member getter | `:45-48` | `bool RegisterMember(const String& name, ReturnType (Object::*get)())` |
| getter+setter | `:61-72` | `bool RegisterMember(const String& name, ReturnType (Object::*get)(), void (Object::*set)(AssignType))` |
| `operator bool` | `:74` | `explicit operator bool() const` |

Callback typedefs, `Include/RmlUi/Core/DataTypes.h:15-28`:

```cpp
using DataGetFunc = Function<void(Variant&)>;
using DataSetFunc = Function<void(const Variant&)>;
using DataTransformFunc = Function<Variant(const VariantList&)>;
using DataEventFunc = Function<void(DataModelHandle, Event&, const VariantList&)>;

template <typename T> using DataTypeGetFunc = void (*)(const T&, Variant&);
template <typename T> using DataTypeSetFunc = void (*)(T&, const Variant&);
```

Note `DataTypeGetFunc`/`DataTypeSetFunc` are **raw function pointers**, not `std::function` — so
`RegisterScalar` cannot take a capturing lambda.

### 2.1 Ownership and lifetime — the crash table

`DataVariable` is exactly two raw pointers and owns neither (`Include/RmlUi/Core/DataVariable.h:20-38`):

```cpp
	DataVariable(VariableDefinition* definition, void* ptr) : definition(definition), ptr(ptr) {}
	...
	VariableDefinition* definition = nullptr;
	void* ptr = nullptr;
```

| API | What is stored where | Caller must keep alive |
|---|---|---|
| `Bind<T>(name, T* ptr)` | `DataVariable{ type_register->GetDefinition<T>(), (void*)ptr }` copied into `DataModel::variables` (`DataModelHandle.h:45`, `DataModel.cpp:132`) | **`*ptr`, for as long as the model lives.** Raw pointer, never revalidated. |
| `Bind<T*>`, `Bind<UniquePtr<T>>`, `Bind<SharedPtr<T>>` | `ptr` = address of *the pointer variable*; a `PointerDefinition<T>` wrapper dereferences it on every access (`DataTypes.h:51-68`, `DataVariable.h:187-194`) | **the pointer variable itself.** The pointee is re-read each access, so it may legitimately change. See the null-pointer trap in §8.3. |
| `BindFunc` | a `FuncDefinition` holding **copies** of both `std::function`s, owned by the model in `function_variable_definitions`; bound `ptr` is `nullptr` (`DataModel.cpp:142-156`) | whatever the lambdas capture. |
| `BindEventCallback` | `DataEventFunc` **moved** into `DataModel::event_callbacks` (`DataModel.cpp:173`) | whatever the callback captures. |
| `BindEventCallback<T>(name, memfn, T* obj)` | a lambda capturing the raw `T* object_pointer` (`DataModelHandle.h:58-60`) | **`*object_pointer`, for as long as the model lives.** |
| `BindCustomDataVariable(name, dv)` | the 16-byte `{definition, ptr}` pair, by value | **both the `VariableDefinition` object and the `void*` target.** Nothing owns the definition. |
| `RegisterScalar<T>` | `ScalarFuncDefinition<T>` owned by the `DataTypeRegister` (`DataModelHandle.h:123-125`) | the type register; the func pointers are static. |
| `RegisterStruct<T>` | `StructDefinition` owned by the type register (`DataModelHandle.h:156-159`); returns a `StructHandle` holding **raw** `DataTypeRegister*` + `StructDefinition*` (`DataStructHandle.h:109-110`) | the type register. |
| `StructHandle::RegisterMember` | a member definition owned by `StructDefinition::members` (`DataVariable.h:130`, `DataVariable.cpp:106-112`), each holding a **raw** `VariableDefinition*` to the member's type (`DataVariable.h:184`, `:275`) | the type register (transitively). |
| `RegisterArray<Container>` | `ArrayDefinition<Container>` owned by the type register, holding a **raw** `VariableDefinition* underlying_definition` (`DataVariable.h:136-138`, `:166`) | the type register. |
| `RegisterCustomDataVariableDefinition<T>` | your `UniquePtr<VariableDefinition>` **moved** into the type register (`DataModelHandle.h:138-141`) | nothing — but see §8.5, the register is never cleared. |
| `RegisterTransformFunc` | `DataTransformFunc` moved into `TransformFuncRegister::transform_functions` inside the `DataTypeRegister` (`DataModelHandle.h:96-99`, `DataTypeRegister.cpp:79-88`) | whatever the func captures. **No unregister.** |

**The one-line answer to "which bindings retain a raw pointer into caller memory":** `Bind<T>` and
`BindCustomDataVariable` (the `void*`), the `BindEventCallback` member-function overload (the
`T* object_pointer`), and every definition object, which are raw `VariableDefinition*` links into
the `DataTypeRegister`. Everything function-shaped copies/moves the `std::function` and therefore
only leaks the lifetime of whatever the lambda captured.

### 2.2 Name legality — applies to `Bind*` and `BindEventCallback`

`Source/Core/DataModel.cpp:51-74`:

```cpp
static const char* LegalVariableName(const String& name)
{
	static SmallUnorderedSet<String> reserved_names{"it", "it_index", "ev", "true", "false", "size", "literal"};

	if (name.empty())
		return "Name cannot be empty.";

	const String name_lower = StringUtilities::ToLower(name);

	const char first = name_lower.front();
	if (!(first >= 'a' && first <= 'z'))
		return "First character must be 'a-z' or 'A-Z'.";

	for (const char c : name_lower)
	{
		if (!(c == '_' || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
			return "Name must strictly contain characters a-z, A-Z, 0-9 and under_score.";
	}

	if (reserved_names.count(name_lower) == 1)
		return "Name is reserved.";

	return nullptr;
}
```

Checked by `BindVariable` (`:119`) and `BindEventCallback` (`:160`) — **but not by `BindFunc`**,
which reaches `BindVariable` at `:155` and so is checked indirectly, and *not* by
`DataModel::BindFunc`'s own duplicate check at `:144`. Failures are `LT_WARNING` + `return false`;
they never throw and never assert.

The check is case-**insensitive** (it lowercases first), but the *storage and lookup* are
case-**sensitive**: `variables.emplace(name, variable)` at `:132` uses the original spelling, and
`ResolveAddress` does `variables.find(first_name)` at `:234` with the exact string from the RML.
Consequences: you cannot bind `Size` or `IT`, and if you bind `Health` you must write `Health` in
the document.

---

## 3. What types RmlUi can represent — and the dynamic-type question

### 3.1 The machinery

Three layers, and it is essential to see that they are separable:

**Layer 1 — the runtime core.** `VariableDefinition` (`Include/RmlUi/Core/DataVariable.h:46-64`) is a
plain polymorphic class. It is **not a template**:

```cpp
class RMLUICORE_API VariableDefinition : public NonCopyMoveable {
public:
	virtual ~VariableDefinition() = default;
	DataVariableType Type() const { return type; }

	virtual bool Get(void* ptr, Variant& variant);
	virtual bool Set(void* ptr, const Variant& variant);

	virtual int Size(void* ptr);
	virtual DataVariable Child(void* ptr, const DataAddressEntry& address);

	virtual StringList ReflectMemberNames();

protected:
	VariableDefinition(DataVariableType type) : type(type) {}

private:
	DataVariableType type;
};
```

`enum class DataVariableType { Scalar, Array, Struct };` — `DataVariable.h:12`. The base
implementations all just log a warning and fail (`Source/Core/DataVariable.cpp:30-55`), so you
override only the ones your kind needs.

Everything the rest of the engine does with data goes through `DataVariable`, which forwards to the
virtuals (`Source/Core/DataVariable.cpp:5-28`):

```cpp
bool DataVariable::Get(Variant& variant) const   { return definition->Get(ptr, variant); }
bool DataVariable::Set(const Variant& variant)   { return definition->Set(ptr, variant); }
int  DataVariable::Size() const                  { return definition->Size(ptr); }
DataVariable DataVariable::Child(const DataAddressEntry& address) const { return definition->Child(ptr, address); }
DataVariableType DataVariable::Type() const      { return definition->Type(); }
```

Address resolution is string-driven at runtime: `DataModel::GetVariable`
(`Source/Core/DataModel.cpp:275-302`) does one hash lookup for `address[0].name` and then walks
`variable = variable.Child(address[i])` — a chain of virtual calls. `DataAddressEntry` is
`{String name; int index;}` with `index == -1` for named entries
(`Include/RmlUi/Core/DataTypes.h:37-42`).

**Layer 2 — the built-in definition kinds** (all in `DataVariable.h`):
`ScalarDefinition<T>` `:69-80`, `FuncDefinition` `:82-92`, `ScalarFuncDefinition<T>` `:94-117`,
`StructDefinition` `:119-131`, `ArrayDefinition<Container>` `:133-167`, `BasePointerDefinition`
`:169-185`, `PointerDefinition<T>` `:187-194`, `MemberObjectDefinition<Object,MemberType>` `:196-208`,
`MemberGetFuncDefinition` `:210-228`, `MemberScalarGetSetFuncDefinition` `:230-278`. Only three of
these are non-template (`FuncDefinition`, `StructDefinition`, `BasePointerDefinition`).

**Layer 3 — the compile-time index.** `DataTypeRegister` maps `FamilyId → UniquePtr<VariableDefinition>`
(`Include/RmlUi/Core/DataTypeRegister.h:112`) and `FamilyId` comes from
`Family<T>::Id()` (`Include/RmlUi/Core/Traits.h:51-61`), a per-type function-local static counter
(`Source/Core/Traits.cpp:5-9`). `GetDefinition<T>()` (`DataTypeRegister.h:35-107`) has three
`if constexpr` branches: builtin scalar (auto-registered on demand), non-scalar (must have been
registered, else `nullptr` + type error), pointer (auto-wrapped).

`is_builtin_data_scalar_v` is the boundary of "free" types
(`Include/RmlUi/Core/DataTypeRegister.h:12-13`):

```cpp
template <class T>
constexpr bool is_builtin_data_scalar_v = std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_same_v<std::remove_const_t<T>, String>;
```

so all arithmetic types, all enums, and `Rml::String` (= `std::string`,
`Include/RmlUi/Config/Config.h:108`) need no registration. `const` is rejected by a `static_assert`
at `DataTypeRegister.h:42`.

Scalar values ultimately live in `Rml::Variant`, whose closed type list is at
`Include/RmlUi/Core/Variant.h:21-48` (`BOOL BYTE CHAR FLOAT DOUBLE INT INT64 UINT UINT64 STRING
VECTOR2 VECTOR3 VECTOR4 COLOURF COLOURB … VOIDPTR`). That is the true scalar boundary.

### 3.2 The answer: **the API is NOT compile-time only.**

The template layer is a convenience over a fully runtime-dispatched core, and there is a public,
non-template door into that core. Two lines decide it.

`Include/RmlUi/Core/DataModelHandle.h:63-65`:

```cpp
	// Bind a user-declared DataVariable.
	// For advanced use cases, such as binding variables to a custom 'VariableDefinition'.
	bool BindCustomDataVariable(const String& name, DataVariable data_variable) { return BindVariable(name, data_variable); }
```

`Include/RmlUi/Core/DataVariable.h:23`:

```cpp
	DataVariable(VariableDefinition* definition, void* ptr) : definition(definition), ptr(ptr) {}
```

`DataVariable`'s constructor is public, `BindCustomDataVariable` is a non-template public member, and
`BindVariable` → `DataModel::BindVariable` only validates the name and the non-nullness of the
definition (`Source/Core/DataModel.cpp:117-140`). **`Family<T>`, `DataTypeRegister::GetDefinition<T>`
and the whole compile-time index are bypassed entirely on this path.**

The runtime dispatch point for a dynamically described struct is
`StructDefinition::Child` (`Source/Core/DataVariable.cpp:76-95`) — note what it returns:

```cpp
DataVariable StructDefinition::Child(void* ptr, const DataAddressEntry& address)
{
	const String& name = address.name;
	...
	auto it = members.find(name);
	if (it == members.end())
	{
		Log::Message(Log::LT_WARNING, "Member %s not found in data struct.", name.c_str());
		return DataVariable();
	}

	VariableDefinition* next_definition = it->second.get();

	return DataVariable(next_definition, ptr);
}
```

It hands the **parent's** `ptr` to the member's definition; the offsetting is done later by
`MemberObjectDefinition::DereferencePointer` (`DataVariable.h:204`):
`return &(static_cast<Object*>(base_ptr)->*member_ptr);`. Meanwhile `ArrayDefinition::Child`
(`DataVariable.h:143-163`) computes the child address itself and returns
`DataVariable(underlying_definition, &(*it))`. **Either idiom is available to a custom definition** —
you may return a new pointer, or the same pointer with a different definition.

### 3.3 What a VaCuus / UE-reflection adapter has to be

Because `Child()` takes `(void* ptr, const DataAddressEntry&)` and returns
`DataVariable{VariableDefinition*, void*}`, a type whose fields are discovered at runtime maps
cleanly. Concretely:

1. **Three definition classes**, hand-written, non-template:
   - `FVcStructDefinition : VariableDefinition(DataVariableType::Struct)` holding a `UStruct*`.
     `Child(ptr, addr)` looks up `addr.name` among the `FProperty`s and returns
     `DataVariable(propertyDefinition, ptr)`.
     It should also override `ReflectMemberNames()` (`DataVariable.h:57`) so any future inspector
     works — see `Source/Debugger/ElementDataModels.cpp:58-74` for how RmlUi consumes it.
   - `FVcPropertyDefinition` — mirrors `MemberObjectDefinition`: derive from `BasePointerDefinition`
     and implement `void* DereferencePointer(void* base)` as
     `Property->ContainerPtrToValuePtr<void>(base)`. You get `Get/Set/Size/Child/ReflectMemberNames`
     forwarding and the null-guard for free (`Source/Core/DataVariable.cpp:134-169`).
   - `FVcArrayDefinition : VariableDefinition(DataVariableType::Array)` over `FScriptArrayHelper`:
     override `Size(void*)` and `Child(void*, addr)`.
   - Plus one scalar definition per `Variant` mapping (int/float/double/bool/`FString`→`Rml::String`),
     or one that switches on the `FProperty` class.
2. **A definition registry owned by the adapter**, e.g. `TMap<FProperty*, TUniquePtr<VariableDefinition>>`
   and `TMap<UStruct*, TUniquePtr<FVcStructDefinition>>`, because the `VariableDefinition*` returned
   from `Child()` must remain valid for as long as the model can be read, and nothing in RmlUi owns
   it on the `BindCustomDataVariable` path.
3. **Root binding:** `constructor.BindCustomDataVariable("player", DataVariable(structDef, shadowPtr))`.
   No `RegisterStruct`, no `RegisterMember`, no `Family<T>`.

**Cost:** roughly 4 small classes plus a lazily-populated registry. There is no per-`USTRUCT`
codegen, no template instantiation per game type, and no need to enumerate types at compile time.
This is the cheap shape, and it is available.

**One caveat that shapes the architecture more than the type system does:** `void* ptr` is stored
once, at bind time, and read on the UI thread inside `Context::Update()`. It must therefore point at
memory the UI thread owns and that UE's GC cannot move or free. **[inference, but strongly implied]**
That means binding to a *shadow/snapshot buffer* owned by the UI thread and published to by the game
thread, not to a live `UObject`'s property storage. See §7.

**If you nevertheless want the templated path** (for a fixed set of C++ mirror structs rather than
runtime reflection), the cost is: one `RegisterStruct<T>()` + one `RegisterMember` per field per
type, executed once per `DataTypeRegister`, with the double-registration crash in §8.5 to guard
against.

### 3.4 Types that do not fit, and their adapters

- **Non-contiguous / computed values.** `Child()` must return a *real address*. If a value has no
  address, expose it as a `Scalar` whose `Get(void* ptr, Variant&)` computes from the parent pointer
  — exactly what `MemberScalarGetSetFuncDefinition` does (`DataVariable.h:239-272`) — or use
  `BindFunc`, whose `FuncDefinition` ignores `ptr` entirely (`Source/Core/DataVariable.cpp:118-132`).
- **Containers.** `RegisterArray<Container>` requires only `size()` and `begin()`
  (`DataModelHandle.h:88-89` doc comment; the code uses `->size()` at `DataVariable.h:140` and
  `->begin()` + `std::advance` at `:158-159`). `TArray` satisfies both — but `Container::value_type`
  must also exist (`DataModelHandle.h:172`), which `TArray` provides as `ElementType`, not
  `value_type`. **[inference]** so `RegisterArray<TArray<T>>` will not compile as-is; write a custom
  array definition instead.
- **Maps / dictionaries.** No `DataVariableType` for them. Model them as an array of key/value
  structs, or as a struct with dynamic `Child()` lookup by name.
- **Anything outside the `Variant` type list** (`Variant.h:21-48`) must be converted to a string or a
  number at the `Get`/`Set` boundary.

---

## 4. Mutation and dirtying

### 4.1 The dirty API

```cpp
void DataModel::DirtyVariable(const String& variable_name)
{
	RMLUI_ASSERTMSG(LegalVariableName(variable_name) == nullptr, "Illegal variable name provided. Only top-level variables can be dirtied.");
	RMLUI_ASSERTMSG(allow_missing_variables || variables.count(variable_name) == 1,
		"In DirtyVariable: Variable name not found among added variables.");
	dirty_variables.emplace(variable_name);
}

bool DataModel::IsVariableDirty(const String& variable_name) const
{
	RMLUI_ASSERTMSG(LegalVariableName(variable_name) == nullptr, "Illegal variable name provided. Only top-level variables can be dirtied.");
	return dirty_variables.count(variable_name) == 1;
}

void DataModel::DirtyAllVariables()
{
	dirty_variables.reserve(variables.size());
	for (const auto& variable : variables)
	{
		dirty_variables.emplace(variable.first);
	}
}
```
— `Source/Core/DataModel.cpp:325-346`.

- **Only top-level names.** `DirtyVariable("player.health")` fails `LegalVariableName` (the `.` is
  illegal, `:66`). In a build with asserts it fires; **in a shipping/development UE build it is
  compiled out** (§7.4) and the malformed name is silently inserted into `dirty_variables` where it
  matches nothing. Same for `DirtyVariable("scores[3]")`.
- `DirtyVariables` is `SmallUnorderedSet<String>` (`Include/RmlUi/Core/DataTypes.h:35`) =
  `itlib::flat_set<String>` (`Include/RmlUi/Config/Config.h:91`) — a *sorted vector*. So
  `DirtyVariable` is O(log n) binary search plus an O(n) memmove plus a `String` copy. Fine for tens
  of names; do not treat it as O(1).

### 4.2 When re-evaluation happens

**Lazily, inside `Context::Update()`, never eagerly.** `Source/Core/Context.cpp:195-197`:

```cpp
	// Update all the data models before updating properties and layout.
	for (auto& data_model : data_models)
		data_model.second->Update(true);
```

That is the only per-frame call. The other call site is `Context::LoadDocument`
(`Source/Core/Context.cpp:302-306`), with `clear_dirty_variables == false`.

`DataModel::Update` (`Source/Core/DataModel.cpp:373-381`) is a thin wrapper:

```cpp
	const bool result = views->Update(*this, dirty_variables);

	if (clear_dirty_variables)
		dirty_variables.clear();

	return result;
```

The return value is **ignored at both call sites** — nothing in Core consumes it.

### 4.3 The update walk — `DataViews::Update`

`Source/Core/DataView.cpp:62-136` is the whole thing. The shape:

```cpp
	for (int i = 0; (i == 0 || !views_to_add.empty() || num_dirty_variables_prev != dirty_variables.size()) && i < 10; i++)
	{
		num_dirty_variables_prev = dirty_variables.size();

		Vector<DataView*> dirty_views;

		if (!views_to_add.empty())
		{
			views.reserve(views.size() + views_to_add.size());
			for (auto&& view : views_to_add)
			{
				dirty_views.push_back(view.get());
				for (const String& variable_name : view->GetVariableNameList())
					name_view_map.emplace(variable_name, view.get());

				views.push_back(std::move(view));
			}
			views_to_add.clear();
		}

		for (const String& variable_name : dirty_variables)
		{
			auto pair = name_view_map.equal_range(variable_name);
			for (auto it = pair.first; it != pair.second; ++it)
				dirty_views.push_back(it->second);
		}

		// Remove duplicate entries
		std::sort(dirty_views.begin(), dirty_views.end());
		auto it_remove = std::unique(dirty_views.begin(), dirty_views.end());
		dirty_views.erase(it_remove, dirty_views.end());

		// Sort by the element's depth in the document tree ...
		std::sort(dirty_views.begin(), dirty_views.end(), [](auto&& left, auto&& right) { return left->GetSortOrder() < right->GetSortOrder(); });

		for (DataView* view : dirty_views)
		{
			...
			if (view->IsValid())
				result |= view->Update(model);
		}

		// Destroy views marked for destruction
		// @performance: Horrible...
		if (!views_to_remove.empty())
		{
			for (const auto& view : views_to_remove)
			{
				for (auto it = name_view_map.begin(); it != name_view_map.end();)
				{
					if (it->second == view.get())
						it = name_view_map.erase(it);
					else
						++it;
				}
			}

			views_to_remove.clear();
		}
	}
```

Cost shape, precisely:

- **Idle cost is near-zero.** With no dirty variables and nothing to add, the body runs exactly once,
  builds an empty `dirty_views`, does two sorts of an empty vector, and the loop condition fails on
  `i == 1`. All models in the context pay this every frame.
- **Per dirty variable:** one `equal_range` on `name_view_map`, a `std::unordered_multimap<String, DataView*>`
  (`Source/Core/DataView.h:96-97`; `UnorderedMultimap` = `std::unordered_multimap`,
  `Config.h:66`), then push-back of every matching view.
- **Per update pass:** two `std::sort`s over the collected view pointers — `O(V log V)` where V is
  the number of views touched, *not* the number of views total.
- **Per view:** `DataView::Update` builds a `DataExpressionInterface` and runs the compiled program
  (§6). Each `Instruction::Variable` costs one robin_hood hash lookup on the root name plus a chain
  of virtual `Child()` calls plus `Variant` conversion.
- **Per DOM write:** every default view compares before writing.
  `DataViewAttribute::Update` `DataViewDefault.cpp:79`, `DataViewStyle::Update` `:168`,
  `DataViewClass::Update` `:191`, `DataViewRml::Update` `:212`, `DataViewText::Update` `:354`.
  So a dirty variable whose value did not change costs expression evaluation but no DOM churn.
- **The re-entrancy loop is capped at 10 iterations** (`:70`). A view's `Update` can add views
  (`data-for` creating rows) or dirty more variables (an assignment via `data-event`), and the loop
  re-runs to converge. Beyond 10 rounds, the remainder waits for the next frame.
- **View removal is quadratic** and self-flagged: `// @performance: Horrible...` (`:117`). Each
  removed view triggers a full linear scan of `name_view_map`. This fires on every `data-for` shrink
  and on every element removal in a data-bound subtree.

**Granularity is per top-level name only.** `DataExpression::GetVariableNameList`
(`Source/Core/DataExpression.cpp:1145-1155`) reduces every address to `address[0].name`:

```cpp
	for (const DataAddress& address : addresses)
	{
		if (!address.empty())
			list.push_back(address[0].name);
	}
```

So dirtying a bound struct re-evaluates every view that reads *any* member of it, and dirtying a
bound array re-evaluates every view in every row (§5).

### 4.4 Ordering

`DataView::GetSortOrder` (`Source/Core/DataView.cpp:17-20`, built in the ctor at `:27-36`) is
`bias + 1000 + 2000 * depth`, so views are updated **shallowest first** — deliberate, so that
`data-for` adds/removes rows before the rows' own views run (`DataView.cpp:102-104`). Two view types
use a bias: `data-value` (+100) and `data-checked` (+110), `Source/Core/DataViewDefault.cpp:15-19`.

---

## 5. Arrays and containers

### 5.1 Size vs. element change

There is **no automatic size-change detection.** `DataViewFor::Update` reads `variable.Size()`
(`Source/Core/DataViewDefault.cpp:503`), but it only runs when the container's *root* name is in
`dirty_variables` — `DataViewFor::GetVariableNameList` returns exactly one name
(`DataViewDefault.cpp:545-549`):

```cpp
	RMLUI_ASSERT(!container_address.empty());
	return StringList{container_address.front().name};
```

So both a size change and an element change are communicated the same way: **`DirtyVariable(root)`**.
If you resize a bound `std::vector` and forget to dirty it, the row count never updates.

`{{ items.size }}` works, via a special case inside `ArrayDefinition::Child`
(`Include/RmlUi/Core/DataVariable.h:147-155`):

```cpp
		const int container_size = int(ptr->size());
		if (index < 0 || index >= container_size)
		{
			if (address.name == "size")
				return MakeLiteralIntVariable(container_size);

			Log::Message(Log::LT_WARNING, "Data array index out of bounds.");
			return DataVariable();
		}
```

Note it is reached because `DataAddressEntry("size")` has `index == -1`
(`Include/RmlUi/Core/DataTypes.h:38`). **A hand-written array definition must re-implement this
`"size"` case itself or `.size` will not work.**

### 5.2 What `data-for` actually does on re-evaluation

`Source/Core/DataViewDefault.cpp:496-543`:

```cpp
	bool result = false;
	const int size = variable.Size();
	const int num_elements = (int)elements.size();
	Element* element = GetElement();

	for (int i = 0; i < Math::Max(size, num_elements); i++)
	{
		if (i >= num_elements)
		{
			ElementPtr new_element_ptr = Factory::InstanceElement(nullptr, element->GetTagName(), element->GetTagName(), attributes);

			DataAddress iterator_address;
			iterator_address.reserve(container_address.size() + 1);
			iterator_address = container_address;
			iterator_address.push_back(DataAddressEntry(i));

			DataAddress iterator_index_address = {{"literal"}, {"int"}, {i}};

			model.InsertAlias(new_element_ptr.get(), iterator_name, std::move(iterator_address));
			model.InsertAlias(new_element_ptr.get(), iterator_index_name, std::move(iterator_index_address));

			Element* new_element = element->GetParentNode()->InsertBefore(std::move(new_element_ptr), element);
			elements.push_back(new_element);

			const String* rml_contents = RMLContents();
			elements[i]->SetInnerRML(rml_contents ? *rml_contents : "");
			...
		}
		if (i >= size)
		{
			model.EraseAliases(elements[i]);
			elements[i]->GetParentNode()->RemoveChild(elements[i]).reset();
			elements[i] = nullptr;
		}
	}

	if (num_elements > size)
		elements.resize(size);

	return result;
```

**It neither rebuilds nor diffs — it reuses, and only appends/truncates at the tail.** Indices
`[0, min(size, num_elements))` are left completely alone. Each row's alias is bound to a fixed index
`i` at creation and never changes.

Practical consequences for a 200-row scoreboard:

- **Value change in one row:** you must `DirtyVariable("scores")`, which re-evaluates *every* view in
  *every* row (all 200 rows' expressions run). DOM writes happen only for rows whose rendered value
  actually changed, because of the compare-before-write in each view. So: **O(all bindings under the
  array) expression evaluations, O(changed) DOM writes.** Cheap-ish, but not free — a 200×5 table is
  1000 expression runs per dirty.
- **Append one row:** one `Factory::InstanceElement` + `InsertBefore` + `SetInnerRML` (which parses
  the row's RML *again*, per row — `RMLContents()` returns the raw stored inner RML,
  `DataViewDefault.cpp:556-572`) and then the new subtree's views are created and updated. Existing
  rows are untouched structurally but still re-evaluate.
- **Insert at the front:** structurally cheap (one append at the tail), semantically expensive —
  every row's aliased index now points at different data, so all 200 rows re-render their text.
- **Shrink:** `RemoveChild(...).reset()` destroys elements immediately, each of which walks
  `DataViews::OnElementRemove` (linear in total views, `DataView.cpp:47-60`) and then the quadratic
  `name_view_map` cleanup (`:118-132`). **This is the expensive direction.**

Rows are inserted as **siblings** of the `data-for` element (`element->GetParentNode()->InsertBefore(..., element)`,
`:523`), and the template element itself is hidden with `display: none` at
`Initialize` time (`:474`).

### 5.3 `data-for` syntax and setup

`DataViewFor::Initialize` (`:429-494`): the expression is `[iterator[, index]] : container`, split on
`:` then on `,`. Defaults are `it` and `it_index` (`:462-466`). Attributes are copied to each row
except `data-for` and `rmlui-inner-rml` (`:486-491`). The raw inner RML is captured by the XML parser
into the `rmlui-inner-rml` attribute (`Source/Core/XMLNodeHandlerDefault.cpp:50-55`), triggered
because `data-for` was registered as a *structural* view (`Source/Core/Factory.cpp:247`,
`:561-572`) and structural attribute names are registered with the parser
(`Source/Core/BaseXMLParser.cpp:338-339`).

Alias resolution is an ancestor walk: `DataModel::ResolveAddress`
(`Source/Core/DataModel.cpp:238-265`) walks up while `ancestor->GetDataModel() == this`, replacing
the first address entry with the alias's full address. That is why `{{it.name}}` inside a row
resolves to `scores[3].name` and reports `scores` as its dependency.

---

## 6. Expressions and events

### 6.1 The expression language, from the parser

Grammar, in precedence order (lowest first), `Source/Core/DataExpression.cpp:286-291` and the
implementations at `:432-556`:

| Level | Function | Operators |
|---|---|---|
| Expression | `:432-472` | `&&`, `\|\|`, `\|` (transform pipe), `? :` |
| Relational | `:474-490` | `==`, `!=`, `<`, `<=`, `>`, `>=` |
| Additive | `:492-506` | `+`, `-` |
| Term | `:508-522` | `*`, `/` |
| Factor | `:523-556` | `( … )`, `'string'`, `!`, number literal, variable or function call |

Semantics, from the interpreter (`:947-1100`):

- **All arithmetic is `double`.** `Subtract/Multiply/Divide/Less/LessEq/Greater/GreaterEq` all do
  `L.Get<double>() OP R.Get<double>()` (`:1007-1016`). There is no integer arithmetic and no
  division-by-zero check.
- **`+` and `==`/`!=` are string-aware:** if *either* side is `Variant::STRING` they operate on
  strings, otherwise on doubles (`:998-1004`, `:1018-1032`, helper `AnyString` at `:949`). So
  `'a' + 1` concatenates and `1 + 1` adds — which means a numeric-looking bound `String` silently
  concatenates instead of adding.
- **Relational operators are never string-aware** — `'b' < 'a'` compares `0.0 < 0.0`.
- **Booleans:** `!`, `&&`, `||` via `Variant::Get<bool>()` (`:1010-1012`). Literals `true`/`false` are
  recognised in `VariableOrFunction` (`:668-671`).
- **String literals are single-quoted**, with `\'` and `\\` escapes (`:570-594`). No double-quoted
  form — double quotes are the RML attribute delimiter.
- **Transform functions**: `func(a, b)` or the pipe form `value | func(args)` where the piped value
  becomes the first argument (`:442-467`, `:837-880`). Builtins registered in the
  `DataTypeRegister` constructor (`Source/Core/DataTypeRegister.cpp:11-74`): **`to_lower`,
  `to_upper`, `format(number, precision[, remove_trailing_zeros])`, `round`** — that is the entire
  built-in set.
- **Dynamic indexing**: `items[expr]` compiles to string concatenation plus
  `Instruction::DynamicVariable`, which re-parses the address at run time
  (`:596-655`, `:681-701`, `:980-988`). Static `items[3].name` is resolved once at parse time to a
  fixed address (`:689-693`). Both the container root and any variables in the index expression are
  registered as dependencies (`:697` and, for the index, via `Expression(parser)` at `:624` →
  `VariableGetSet` at `:255-266`).
- **Ternary** is compiled to real jumps with patching (`:818-835`, `:1086-1096`) — the untaken branch
  is not evaluated.
- Execution is by a tiny stack machine; each `DataExpression::Run` constructs a fresh
  `DataInterpreter` with a `Vector<Variant> stack` (`:940-945`, `:1134-1143`) — i.e. a potential heap
  allocation per view per update.

### 6.2 `{{ … }}` in text

`DataViewText::Initialize` (`Source/Core/DataViewDefault.cpp:274-339`) scans the text node for
double-curly regions via `XMLParseTools::ParseDataBrackets`
(`Source/Core/XMLParseTools.cpp:125-163`), compiles one `DataExpression` per region, and stores the
literal text between them. The `data-text` attribute is added *automatically* by the text instancer
when curly brackets are present (`Source/Core/Factory.cpp:390-392`).

`ParseDataBrackets` rejects nested `{{`, a lone `}`, and an unterminated expression at an XML tag
boundary, each with a specific error string (`XMLParseTools.cpp:125-163`).

`DataViewText::Update` (`:341-380`) re-runs every embedded expression, compares each entry's
rendered string, and only if something changed rebuilds the whole text and calls
`SystemInterface::TranslateString` then `ElementText::SetText` (`:362-372`). Initial entry values are
seeded with the sentinel `"#rmlui#"` (`:313`) so the first update always writes.

### 6.3 `data-model` scoping

`data-model="name"` is read only in `Element::SetParent` (`Source/Core/Element.cpp:2203`), and
`Element::SetDataModel` propagates to all children (`:2164-2165`) but **stops at a nested model**
(`:2152-2154`):

```cpp
	// stop descent if a nested data model is encountered
	if (data_model && new_data_model && data_model != new_data_model)
		return;
```

So a subtree with its own `data-model` shadows the outer one, and `ResolveAddress`'s alias walk
likewise stops at the model boundary (`Source/Core/DataModel.cpp:241`).

### 6.4 Views and controllers actually available

Views (`Source/Core/Factory.cpp:236-247`): `data-attr-*`, `data-attrif-*`, `data-class-*`, `data-if`,
`data-visible`, `data-rml`, `data-style-*`, `data-text`, `data-value`, `data-checked`,
`data-alias-*`, and the structural `data-for`.
Controllers (`:251-253`): `data-checked`, `data-event-*`, `data-value` — note `data-checked` and
`data-value` are *both* a view and a controller (two-way), and `ApplyDataViewsControllers` instances
both for the same attribute (`Source/Core/ElementUtilities.cpp:428-432`).

### 6.5 `data-event-*`

`DataControllerEvent::Initialize` (`Source/Core/DataControllerDefault.cpp:78-99`) parses the value
as an **assignment expression** (`Parse::Assignment`, `DataExpression.cpp:386-431`), which accepts a
`;`-separated list of either `variable = expression` or `callback(args…)`.

The handler signature is `void(DataModelHandle, Event&, const VariantList&)`
(`Include/RmlUi/Core/DataTypes.h:18`). What it receives:

- the `DataModelHandle` for the model the element is bound to
  (`DataExpression.cpp:1213`: `DataModelHandle handle(data_model);`) — so a handler can dirty
  variables directly;
- the live `Event&`, forwarded from `DataControllerEvent::ProcessEvent`
  (`DataControllerDefault.cpp:101-113`);
- the evaluated arguments as a `VariantList`, popped from the interpreter stack
  (`DataExpression.cpp:1102-1115`).

Event parameters are readable in expressions as `ev.<name>`, special-cased in
`DataExpressionInterface::ParseAddress` and `GetValue`
(`DataExpression.cpp:1161-1183`) — the lookup is against `Event::GetParameters()`, and a missing
parameter yields an empty `Variant` with **no diagnostic**.

Assignment dirties the root name automatically (`DataExpression.cpp:1185-1197`):

```cpp
		if (DataVariable variable = data_model->GetVariable(address))
			result = variable.Set(value);

		if (result)
			data_model->DirtyVariable(address.front().name);
```

`data-value` / `data-checked` do the same on the `change` event
(`Source/Core/DataControllerDefault.cpp:34-60`), reading the event's
`data-binding-override-value` parameter in preference to `value` (`:42-47`).

### 6.6 What silently no-ops vs. what logs

| Situation | Behaviour | Where |
|---|---|---|
| Parse error in an expression | `LT_WARNING` ×3 with a caret pointing at the column; the view/controller is **not added**, plus a second `LT_WARNING` naming the element | `DataExpression.cpp:128-137`; `ElementUtilities.cpp:473`, `:484` |
| Unknown variable name in an expression | `LT_WARNING` "Could not find variable name '…' in data model." then parse error, unless `allow_missing_variables` | `DataModel.cpp:267-272`, `DataExpression.cpp:257-262` |
| Malformed `data-for` syntax | `LT_WARNING` "Invalid syntax in data-for" | `DataViewDefault.cpp:437`, `:448` |
| Array index out of bounds at runtime | `LT_WARNING` "Data array index out of bounds." | `DataVariable.h:154` |
| Struct member not found at runtime | `LT_WARNING` "Member %s not found in data struct." | `DataVariable.cpp:88` |
| Transform function not found | `TransformFuncRegister::Call` returns false → interpreter `Error` → `LT_WARNING` + program dump | `DataTypeRegister.cpp:92-94`, `DataExpression.cpp:1050-1062`, `:927-932` |
| **Unrecognised `data-xyz` attribute** | **nothing at all** — no view, no controller, no log | `ElementUtilities.cpp:428-434` (`if (initializer)` is false), `:457-487` |
| **`data-event-<TypoOrWrongCase>`** | **nothing at all** — a brand-new custom event id is minted and a listener is added for an event that is never dispatched | `DataControllerDefault.cpp:88`, `EventSpecification.cpp:135-144`, `:95-118` |
| `data-alias-*` after Initialize | `Update` is a hard `return false;` — aliases are set up once, at Initialize | `DataViewDefault.cpp:581-584` |
| `DirtyVariable` with an unknown or non-top-level name | assert in a debug build, **silent no-op otherwise** | `DataModel.cpp:327-330` + §7.4 |

---

## 7. Thread affinity and reentrancy

### 7.1 There are no thread assertions anywhere in RmlUi

`grep -rin "thread"` over `Source/Core/*.cpp`, `Source/Core/*.h` and `Include/RmlUi/Core/*.h` returns
exactly one hit: a comment on `ObserverPtr` — `Include/RmlUi/Core/ObserverPtr.h:38`,
*"Note: Not thread safe."* No mutexes, no atomics, no thread-id checks. The library will not tell you
when you get this wrong.

### 7.2 Process-wide mutable globals reachable from the data path

| Global | Where | Touched by |
|---|---|---|
| `FamilyBase::GetNewId()` — `static int id = 0; return id++;` | `Source/Core/Traits.cpp:5-9` | first `Family<T>::Id()` for each `T`, i.e. every `Bind<T>`, `RegisterStruct<T>`, `RegisterArray<C>`. **Not atomic** — two threads first-touching two different `T`s race here. |
| `event_specification_data` (specification vector + name→id map) | `Source/Core/EventSpecification.cpp:95-144` | `DataControllerEvent::Initialize` → `GetIdOrInsert(modifier)` (`DataControllerDefault.cpp:88`) — i.e. **document loading mutates a process-wide registry**. |
| `system_interface` | `Source/Core/Core.cpp:208-216` | `DataViewText::Update` → `GetSystemInterface()->TranslateString` (`DataViewDefault.cpp:368-369`) |
| `factory_data` (view/controller instancer maps) | `Source/Core/Factory.cpp:136-137`, `:561-601` | read on every `ApplyDataViewsControllers`; written by `RegisterDataViewInstancer` |
| `LiteralIntDefinition` function-local static | `Source/Core/DataVariable.cpp:70` | `MakeLiteralIntVariable`, on every `data-for` row creation and every `array.size` read. Stateless, so read-only after magic-static init. |
| `reserved_names` function-local static | `Source/Core/DataModel.cpp:53` | every `LegalVariableName`. Read-only after init. |
| C locale | `Source/Core/Context.cpp:35-57` | RmlUi requires the global `"C"` locale for number formatting/parsing; `format` uses `FormatString` (`DataTypeRegister.cpp:56-58`). The check itself is `#ifdef RMLUI_DEBUG` only. |

**Conclusion for VaCuus:** the process-wide UI thread must be the *only* thread that touches any of
these. `Family<T>::Id()` in particular means even *registering* types from the game thread while the
UI thread loads a document is a data race.

### 7.3 Can a model be mutated while `Context::Update()` is running?

**From another thread: no.** `DataModel::variables` is
`UnorderedMap<String, DataVariable>` = `robin_hood::unordered_flat_map`
(`Source/Core/DataModel.h:62`, `Config.h:83`, `robin_hood.h:2368`) and `dirty_variables` is a
`flat_set` — both are plain non-atomic containers read and written during `DataViews::Update`. Any
concurrent `DirtyVariable` or `Bind*` is a data race.

**From the same thread, re-entrantly: yes, and it is designed for.** A view's `Update` may

- dirty more variables (assignment via a `data-event` handler, or `DataControllerValue::ProcessEvent`
  → `model->DirtyVariable`, `DataControllerDefault.cpp:59`),
- add views (a `data-for` row's `SetInnerRML` → `ApplyDataViewsControllers` → `AddView`),
- remove views and destroy elements (`data-for` shrink),

and `DataViews::Update`'s outer loop re-converges up to 10 times (`DataView.cpp:70`). Insertions into
`dirty_variables` happen *outside* the range-for over it (that loop, `:90-95`, only builds the view
list), so the container is not mutated while iterated. Views destroyed mid-pass stay alive in
`views_to_remove` until the end of the iteration (`:118-131`) and are skipped by the `IsValid()` guard
at `:112`, so no use-after-free.

**What is not safe re-entrantly:** `DataModel::GetEventCallback` returns `&it->second` — a pointer
**into** a `robin_hood::unordered_flat_map` (`Source/Core/DataModel.cpp:304-314`), which is then
invoked at `DataExpression.cpp:1214`:

```cpp
	const DataEventFunc* func = data_model->GetEventCallback(name);
	if (!func || !*func)
		return false;

	DataModelHandle handle(data_model);
	func->operator()(handle, *event, arguments);
```

`unordered_flat_map` stores values inline and relocates them on insert, so **an event handler that
calls `BindEventCallback` on its own model can destroy the `std::function` it is currently executing.**
Narrow, but real.

### 7.4 Asserts are compiled out in every normal UE build

`Include/RmlUi/Core/Platform.h:20-21`:

```cpp
#if !defined NDEBUG && !defined RMLUI_DEBUG
	#define RMLUI_DEBUG
```

`Include/RmlUi/Core/Debug.h:45-52`:

```cpp
#if !defined RMLUI_DEBUG

	#define RMLUI_ASSERT(x)
	#define RMLUI_ASSERTMSG(x, m)
	#define RMLUI_ERROR
	#define RMLUI_ERRORMSG(m)
	#define RMLUI_VERIFY(x) x
	#define RMLUI_ASSERT_NONRECURSIVE
```

And UBT defines `NDEBUG=1` for every configuration that does not use the debug CRT —
`/w/Unreal/UnrealEngine/Engine/Source/Programs/UnrealBuildTool/Configuration/UEBuildPlatform.cs:1344`:

```csharp
				GlobalCompileEnvironment.Definitions.Add("NDEBUG=1"); // the engine doesn't use this, but lots of 3rd party stuff does
```

**So in a normal Development editor build, every `RMLUI_ASSERT` / `RMLUI_ASSERTMSG` / `RMLUI_ERROR`
in the data-binding code is a no-op.** This turns several designed-to-assert failures into silent
no-ops (§4.1) and, worse, `RMLUI_LOG_TYPE_ERROR` — which is `RMLUI_ERRORMSG`
(`Include/RmlUi/Core/DataTypes.h:74`) — means **the entire type-registration error path logs
nothing** in a normal build:

```cpp
#define RMLUI_LOG_TYPE_ERROR(T, msg) RMLUI_ERRORMSG((String(msg) + String("\nT: ") + String(rmlui_type_name<T>())).c_str())
#define RMLUI_LOG_TYPE_ERROR_ASSERT(T, val, msg) RMLUI_ASSERTMSG((val), (String(msg) + String("\nT: ") + String(rmlui_type_name<T>())).c_str())
```

Silent in a normal build: "type T not registered" (`DataTypeRegister.h:62-66`), "Scalar function type
already registered" (`DataModelHandle.h:128`), "Struct type already declared" (`:162`), "Array type
already declared" (`:184`), "Custom data type already registered" (`:142`), "Underlying value type of
array has not been registered" (`:174`). Only `DataModel::BindVariable`'s generic
*"data type not registered"* `LT_WARNING` (`DataModel.cpp:128`) survives, and only when the failure
reaches a bind.

Compounding this: with `RMLUI_CUSTOM_RTTI=1` (set at `Source/VaCuusRml/VaCuusRml.Build.cs:20`),
`rmlui_type_name<T>()` returns the literal string `"(type name unavailable)"`
(`Include/RmlUi/Core/Traits.h:120-124`), so even in a debug build those messages do not name the type.

---

## 8. Traps

**8.1 `RemoveDataModel` permanently orphans already-loaded documents.** See §1.4. There is no
re-attach API — `Element::SetDataModel` is private. For live reload, either keep the model alive
across reloads or reload the documents too.

**8.2 There is no way to unbind a single variable.** `DataModel` has `BindVariable`/`BindFunc`/
`BindEventCallback` and no counterparts (`Source/Core/DataModel.h:25-28`). Re-binding a name logs
`LT_WARNING` "Data model variable with name '…' already exists." and returns false
(`DataModel.cpp:132-137`) — the old binding, pointing at possibly-freed memory, stays.

**8.3 A bound null pointer crashes on some paths and returns cleanly on others.**
`BasePointerDefinition` null-checks its *own* `ptr` and then hands the possibly-null dereferenced
result to the underlying definition (`Source/Core/DataVariable.cpp:138-164`):

```cpp
bool BasePointerDefinition::Get(void* ptr, Variant& variant)
{
	if (!ptr)
		return false;
	return underlying_definition->Get(DereferencePointer(ptr), variant);
}
```

`ScalarDefinition<T>::Get` then does `variant = *static_cast<const T*>(ptr);`
(`Include/RmlUi/Core/DataVariable.h:76`) with **no** null check → `Bind<int*>` on a null pointer
crashes. `MemberScalarGetSetFuncDefinition::Get` calls a member function through the null pointer
(`:248`) → crashes. But `MemberObjectDefinition` *is* a `BasePointerDefinition`, so
`Bind<MyStruct*>` with a null pointer and a plain member access returns `false` cleanly. Do not rely
on the null check being there.

**8.4 `DataVariable::Get`/`Set`/`Size`/`Child`/`Type` do not null-check `definition`.**
`Source/Core/DataVariable.cpp:5-28` unconditionally dereferences. Every caller in Core guards with
`explicit operator bool()` (`DataVariable.h:25`) first; any custom code must too.

**8.5 Double-registering a struct type is a *crash*, not an error.** `RegisterStruct<T>`
(`Include/RmlUi/Core/DataModelHandle.h:151-167`):

```cpp
	const bool inserted = type_register->RegisterDefinition(id, std::move(struct_definition));
	if (!inserted)
	{
		RMLUI_LOG_TYPE_ERROR(T, "Struct type already declared");
		return StructHandle<T>(nullptr, nullptr);
	}
```

The `RMLUI_LOG_TYPE_ERROR` is a no-op in a normal build (§7.4), so you silently get a
`StructHandle<T>(nullptr, nullptr)` — and `StructHandle::RegisterMember` immediately does
`type_register->GetDefinition<MemberType>()` on the null register
(`Include/RmlUi/Core/DataStructHandle.h:125`, `:138`, `:165`). `StructHandle::operator bool` exists
(`:74`) but nothing calls it. **Running your registration function twice — two contexts, a hot
reload, an editor PIE restart — null-derefs.**
Mitigations: check `if (auto h = ctor.RegisterStruct<T>())` at every call site, or give each model
its own `DataTypeRegister` via `CreateDataModel(name, myRegister)`.

**8.6 `DataTypeRegister` can never be cleared, and neither can transform functions.** The class has
only `RegisterDefinition` and `GetDefinition` (`Include/RmlUi/Core/DataTypeRegister.h:24-114`).
Re-registering a transform function name is worse than a no-op
(`Source/Core/DataTypeRegister.cpp:79-88`):

```cpp
	bool inserted = transform_functions.emplace(name, std::move(transform_func)).second;
	if (!inserted)
	{
		Log::Message(Log::LT_ERROR, "Transform function '%s' already exists.", name.c_str());
		RMLUI_ERROR;
	}
```

`LT_ERROR` + assert. Because the default register is per-Context
(`Include/RmlUi/Core/Context.h:385`), any per-reload registration must use a register VaCuus owns.

**8.7 `data-*` attributes are read exactly once, at model-attach time.** `ApplyDataViewsControllers`
is called only from `Element::SetDataModel` (`Source/Core/Element.cpp:2162`). Adding a `data-text` or
`data-if` attribute to a live element with `SetAttribute` creates no view and logs nothing.
Conversely `Element::SetInnerRML` on an attached element *does* produce views, because the new
children go through `AppendChild` → `SetParent` → `SetDataModel`
(`Source/Core/Element.cpp:1342-1370`, `:2206`).

**8.8 Attribute names are case-sensitive; element tag names are not.** The XML parser lowercases tag
names (`Source/Core/XMLParser.cpp:61`, `:74`, `:114`, `:136`, `:167`) but `ReadAttributes`
(`Source/Core/BaseXMLParser.cpp:306-345`) stores attribute names verbatim. Combined with the exact
`name[0]=='d' && … && name[4]=='-'` prefix check and the exact type-name lookup in
`ApplyDataViewsControllers` (`Source/Core/ElementUtilities.cpp:416-432`), **`data-Model`, `data-IF`,
`data-Text` all silently do nothing.** `data-event-Click` is worse: it succeeds and mints a new
custom event id that is never dispatched (§6.6).

**8.9 Data-model variable names are case-sensitive in lookup but case-insensitive in validation.**
See §2.2 — you cannot bind `Size`, and `Health` must be spelled `Health` in the RML.

**8.10 Dirty granularity is per top-level name.** `DirtyVariable("player")` re-evaluates every
expression that reads any `player.*`. There is no way to dirty `player.health` alone — the
`LegalVariableName` guard rejects the dotted form (§4.1), and even if it did not, `name_view_map` is
keyed on root names only (`DataExpression.cpp:1152`).

**8.11 `data-for` leaves its generated rows behind if the template element is removed.**
The rows are siblings, not children (`DataViewDefault.cpp:523`), and `DataViewFor::Release` is
`delete this;` with no cleanup of the `elements` list (`:551-554`). Removing the `data-for` element
from a live document orphans its rows in the DOM. (A whole-document unload is fine.)

**8.12 `DataViewFor::Update` never sets its return value.** `bool result = false;` at
`Source/Core/DataViewDefault.cpp:502`, `return result;` at `:542`, nothing in between assigns it —
so `data-for` always reports "no document change" even when it created or destroyed rows. The value
is currently harmless because `DataViews::Update`'s `result` propagates to `DataModel::Update` whose
return is discarded at both call sites (`Source/Core/Context.cpp:197`, `:306`). Flagging it because
it *looks* like a bug and any future consumer of that return value would be wrong. **[inference]**
I could not diff against upstream (no VCS metadata in the vendored tree), so I cannot say whether
this is a local edit; the data-binding files contain no VaCuus markers or `#if` guards, so it is most
likely upstream as-is.

**8.13 `DataParser::SetProgramState` rewinds `program` but not `variable_addresses`.**
`Source/Core/DataExpression.cpp:235-240`:

```cpp
	void SetProgramState(const ProgramState& state)
	{
		RMLUI_ASSERT(state.program_length <= program.size());
		program.resize(state.program_length);
		program_stack_size = state.stack_size;
	}
```

Used for speculative parsing of `name[…]` (`:607`, `:619`, `:685`, `:691`). If a speculative parse
ever adds an address *and* then rewinds, the stale address survives into `GetVariableNameList()`.
**[inference]** I traced the reachable paths and could not construct a case where it happens — in the
static-index branch `Expression()` is never called, so no address is added. The failure mode would
be an *extra* dependency (over-updating), never a missed one, so it is benign either way. Worth
knowing if you ever extend the parser.

**8.14 The `data-for` `size` special case lives in `ArrayDefinition`, not in the core.** A custom
array definition that forgets `if (address.name == "size")` (`DataVariable.h:151-152`) breaks
`{{ items.size }}` with only an "index out of bounds" warning.

**8.15 `ElementDocument`-level: the model must exist before `LoadDocument`.** §1.4. The failure is a
single `LT_ERROR` at `Source/Core/Element.cpp:2218` and then a fully inert document.

---

## 9. Deltas from upstream in this vendored copy

I have no upstream checkout to diff against, so this is a structural comparison, not a line diff.

- **Root layout is `Backends/ Include/ Source/ LICENSE.txt readme.md VENDORED_SHA.txt`** — no
  `CMakeLists.txt`, `CMake/`, `Samples/`, `Tests/`. **This removes the upstream data-binding unit
  tests and the `databinding` sample**, which would otherwise be the fastest way to answer §10's
  open questions.
- **`Source/` contains only `Core/` and `Debugger/`** — the Lua, SVG and Lottie source trees are gone,
  as stated in the task.
- **But their public headers were left behind:** `Include/RmlUi/Lua/` (7 headers),
  `Include/RmlUi/Lua.h`, `Include/RmlUi/SVG/ElementSVG.h`, `Include/RmlUi/Lottie/ElementLottie.h`.
  Including any of them compiles and then fails to link. Harmless but worth deleting.
- **`Source/Debugger/` is present but not compiled** — no relay files exist for it (§0). The Data
  Models inspector (`Source/Debugger/ElementDataModels.cpp`) is therefore unavailable. If a
  data-model inspector is wanted in the editor, adding those relays is the cheapest route; the two
  introspection hooks it uses are public-ish:
  `Detail::DataModelConstructorAccessor::GetAllVariables` (`Include/RmlUi/Core/DataModelHandle.h:191-195`)
  and `Detail::DataVariableAccessor::GetDefinition` (`Include/RmlUi/Core/DataVariable.h:280-285`).
- **The data-binding sources themselves show no local modification** — grepping the 10 data-binding
  `.cpp`/`.h` files for `VaCuus`, `VACUUS`, `VcRml`, `MODIFIED`, `UE_`, `Unreal` returns nothing.
- **`RMLUI_CUSTOM_RTTI=1`** is a build-level deviation from upstream defaults
  (`Source/VaCuusRml/VaCuusRml.Build.cs:20`), with the type-name consequence noted in §7.4.

---

## 10. Open questions and the experiments that settle them

1. **Does detach/re-append of a `data-model`-carrying child element re-resolve the model after
   `RemoveDataModel` + `CreateDataModel`?** (§1.4, §8.1.)
   *Experiment:* load a document with `<div data-model="m">` inside `<body>`; call
   `RemoveDataModel("m")`, `CreateDataModel("m")` and re-bind; then
   `ElementPtr p = div->GetParentNode()->RemoveChild(div); parent->AppendChild(std::move(p));`
   and check that `div->GetDataModel()` is non-null and that `{{ }}` re-renders after one
   `Context::Update()`.

2. **The exact per-frame cost of a 200-row `data-for` on a single `DirtyVariable`.** The code says
   O(rows × bindings) expression evaluations with compare-before-write; the constant factor
   (`Vector<Variant>` allocation per `DataExpression::Run`, `String` construction in every
   `variant.Get<String>()`) is not readable from source.
   *Experiment:* build a 200×5 table, dirty the array every frame with one changed cell, and profile
   `Context::Update`. Compare against dirtying with zero changed cells to separate evaluation cost
   from DOM cost.

3. **Whether `RegisterArray<TArray<T>>` compiles.** `DataModelHandle.h:172` needs
   `Container::value_type`; `TArray` exposes `ElementType`. Predicted to fail to compile — a
   one-line compile test settles it, and if it fails, §3.3's custom array definition is the answer
   anyway.

4. **Whether `robin_hood::unordered_flat_map` relocation actually breaks the event-callback pointer
   in practice** (§7.3). *Experiment:* bind ~32 event callbacks, then from inside one of them bind a
   33rd, under ASan.

5. **Whether the vendored `DataViewFor::Update` `result` really matches upstream** (§8.12). Settled
   by one `git show 0ae381e:Source/Core/DataViewDefault.cpp` against an upstream clone — outside this
   document's read-only scope.
