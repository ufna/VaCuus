// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusTranslationVariable.h"

#include "VaCuus.h"
#include "VaCuusDefines.h"
#include "VaCuusTranslation.h"
#include "VaCuusUIThread.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>

namespace
{
/** UI-thread observables; see the header for what each is for. */
uint64 GNumGets = 0;
uint64 GNumDirties = 0;

/**
 * THE INTERNED KEY PATHS, and the reason they exist: RmlUi's DataVariable is exactly
 * (VariableDefinition*, void*) (DataVariable.h:20-38) with no room for a string, so a variable
 * standing for the key path "menu.settings.title" has to carry that path as a stable pointer.
 * These FStrings are heap-stable (TUniquePtr elements, so an array grow does not move them) and
 * live until ReleaseAll().
 *
 * IT CANNOT GROW WITHOUT BOUND IN A RUNNING GAME: an entry is minted per DISTINCT key path ever
 * evaluated, which is a property of the documents' markup, not of frames or of table pushes.
 * GetNumInternedKeys() is the observable that proves it.
 */
TArray<TUniquePtr<FString>> GKeyPaths;
TMap<FString, FString*> GKeyPathLookup;

/** "t", once, in RmlUi's encoding — DirtyVariable takes a std::string and this runs per push. */
const Rml::String GReservedNameUtf8 = "t";

/** One-shot diagnostics: these paths are per evaluation, so a repeat would flood the log. */
bool bWarnedBareRoot = false;
bool bWarnedWrite = false;

/**
 * The standalone `vacuus` model per view (see the header). Raw handles into DataModels the
 * contexts own, so an entry MUST be dropped before its context is destroyed — RemoveView does
 * that ahead of Host->Shutdown().
 */
TMap<uint32, Rml::DataModelHandle> GStandaloneModels;

/** The model name a document writes in `data-model=` to reach translations with no game struct. */
const Rml::String GStandaloneModelName = "vacuus";

FString* InternKeyPath(const FString& KeyPath)
{
	if (FString** Existing = GKeyPathLookup.Find(KeyPath))
	{
		return *Existing;
	}

	TUniquePtr<FString> Owned = MakeUnique<FString>(KeyPath);
	FString* Stable = Owned.Get();
	GKeyPaths.Add(MoveTemp(Owned));
	GKeyPathLookup.Add(KeyPath, Stable);
	return Stable;
}

/**
 * The one definition, shared by the root variable and by every key-path variable descended from
 * it — they differ only in the void*. Declared Struct because it has children; Get() is
 * overridden anyway, and RmlUi gates nothing on the type at read time (DataVariable::Get is a
 * straight call through, DataVariable.cpp:5-8).
 */
class FTranslationVariableDefinition final : public Rml::VariableDefinition
{
public:
	FTranslationVariableDefinition()
		: Rml::VariableDefinition(Rml::DataVariableType::Struct)
	{
	}

	bool Get(void* Ptr, Rml::Variant& Variant) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		if (Ptr == nullptr)
		{
			// A bare `{{ t }}` — the root with no key under it. Named once because it is an
			// authoring mistake with no other symptom: the element would simply render empty.
			if (!bWarnedBareRoot)
			{
				bWarnedBareRoot = true;
				UE_LOG(LogVaCuus, Warning,
					TEXT("A document evaluates `{{ t }}` with no key after it; `t` is the translation root, so write ")
					TEXT("`{{ t.some_key }}`. Reported once per process"));
			}
			return false;
		}

		++GNumGets;

		const FString& KeyPath = *static_cast<const FString*>(Ptr);

		// IDENTITY ON A MISS, the contract both other readers already keep
		// (FVaCuusSystemInterface::TranslateString and the `vacuus.translate` thunk): an
		// untranslated key renders as itself rather than as emptiness, so a missing entry is
		// visible on screen instead of silently blanking a label.
		FString Translated;
		if (!FVaCuusTranslationRegistry::TranslateKey(KeyPath, Translated))
		{
			Translated = KeyPath;
		}

		Variant = Rml::String(TCHAR_TO_UTF8(*Translated));
		return true;
	}

	bool Set(void* Ptr, const Rml::Variant& /*Variant*/) override
	{
		// A translation is derived state; there is nothing to write back into. Refused with one
		// named line rather than the base class's generic "values can only be assigned to scalar
		// data types", which would leave the author hunting for which variable it meant.
		if (!bWarnedWrite)
		{
			bWarnedWrite = true;
			UE_LOG(LogVaCuus, Warning,
				TEXT("A two-way binding tried to WRITE to translation key '%s'; translations are read-only. ")
				TEXT("Reported once per process"),
				Ptr != nullptr ? **static_cast<const FString*>(Ptr) : TEXT("<root>"));
		}
		return false;
	}

	Rml::DataVariable Child(void* Ptr, const Rml::DataAddressEntry& Address) override
	{
		check(FVaCuusUIThread::IsInUIThread());

		if (Address.name.empty())
		{
			// An index, i.e. `{{ t[0] }}`. The key space is not an array; an invalid variable is
			// how RmlUi's own definitions answer an address they cannot serve.
			return Rml::DataVariable();
		}

		// THE PREFIX ACCUMULATES because ParseAddress splits on '.' (DataModel.cpp:9-42), so
		// `{{ t.menu.settings.title }}` arrives here three times and only the last call knows
		// the whole key. Each intermediate node is a perfectly usable variable in its own right
		// — `{{ t.menu }}` looks up "menu" — which is why every level interns rather than only
		// the leaf.
		const FString& Name = UTF8_TO_TCHAR(Address.name.c_str());
		FString KeyPath = Ptr != nullptr ? (*static_cast<const FString*>(Ptr) + TEXT(".") + Name) : Name;

		return Rml::DataVariable(this, InternKeyPath(KeyPath));
	}

	Rml::StringList ReflectMemberNames() override
	{
		// Deliberately empty and silent: the key space is open — any name resolves — so there is
		// no member list to reflect. The base class would log a warning saying this is not a
		// struct, which is both untrue and unhelpful.
		return Rml::StringList();
	}
};

/**
 * Function-local static rather than a namespace global: RmlUi keeps a raw VariableDefinition*
 * inside every DataVariable it hands out, so this must outlive every context. A local static is
 * constructed on first use and destroyed after main, which is safe here precisely because it
 * owns nothing that needs the engine alive (contrast FVaCuusDefinitionRegistry, whose
 * TStrongObjectPtr forced an explicit teardown).
 */
FTranslationVariableDefinition& GetDefinition()
{
	static FTranslationVariableDefinition Definition;
	return Definition;
}
}	 // namespace

const TCHAR* const VaCuusTranslationVariable::ReservedName = TEXT("t");

bool VaCuusTranslationVariable::Bind(Rml::DataModelConstructor& Constructor)
{
	check(FVaCuusUIThread::IsInUIThread());

	// Null pointer IS the root: DataVariable's validity is `definition != nullptr` alone
	// (DataVariable.h:24), so a null handle is a legal variable, and it is what Child() reads as
	// "no prefix yet".
	return Constructor.BindCustomDataVariable(GReservedNameUtf8, Rml::DataVariable(&GetDefinition(), nullptr));
}

void VaCuusTranslationVariable::Dirty(Rml::DataModelHandle& Handle)
{
	check(FVaCuusUIThread::IsInUIThread());

	++GNumDirties;
	Handle.DirtyVariable(GReservedNameUtf8);
}

bool VaCuusTranslationVariable::CreateStandaloneModel(uint32 ViewId, Rml::Context& Context)
{
	check(FVaCuusUIThread::IsInUIThread());

	Rml::DataModelConstructor Constructor = Context.CreateDataModel(GStandaloneModelName);
	if (!Constructor)
	{
		// The only way this fails is a name clash inside this context, which means a game bound
		// a model of its own called `vacuus`. RmlUi logs "Data model name 'vacuus' already
		// exists." through FVaCuusSystemInterface; this line says what it costs.
		UE_LOG(LogVaCuus, Error,
			TEXT("View %u already has a data model named '%s', so the standalone translation model was not created; ")
			TEXT("`<body data-model=\"vacuus\">` will address the OTHER model"),
			ViewId, UTF8_TO_TCHAR(GStandaloneModelName.c_str()));
		return false;
	}

	Bind(Constructor);
	GStandaloneModels.Add(ViewId, Constructor.GetModelHandle());
	return true;
}

void VaCuusTranslationVariable::DropStandaloneModel(uint32 ViewId)
{
	check(FVaCuusUIThread::IsInUIThread());
	GStandaloneModels.Remove(ViewId);
}

void VaCuusTranslationVariable::DirtyStandaloneModels()
{
	check(FVaCuusUIThread::IsInUIThread());

	for (TPair<uint32, Rml::DataModelHandle>& Pair : GStandaloneModels)
	{
		Dirty(Pair.Value);
	}
}

int32 VaCuusTranslationVariable::GetNumStandaloneModels()
{
	check(FVaCuusUIThread::IsInUIThread());
	return GStandaloneModels.Num();
}

uint64 VaCuusTranslationVariable::GetNumGets()
{
	check(FVaCuusUIThread::IsInUIThread());
	return GNumGets;
}

uint64 VaCuusTranslationVariable::GetNumDirties()
{
	check(FVaCuusUIThread::IsInUIThread());
	return GNumDirties;
}

int32 VaCuusTranslationVariable::GetNumInternedKeys()
{
	check(FVaCuusUIThread::IsInUIThread());
	return GKeyPaths.Num();
}

int32 VaCuusTranslationVariable::ReleaseAll()
{
	check(FVaCuusUIThread::IsInUIThread());

	// EVERY VIEW IS GONE BY NOW, so every standalone handle has already been dropped by
	// RemoveView. A leftover means a context was destroyed without its view being removed, and
	// the handles left behind point into freed DataModels -- exactly the shape of bug this
	// ordering exists to prevent, so it is said out loud rather than quietly cleaned up.
	if (!GStandaloneModels.IsEmpty())
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("%d standalone translation model handle(s) outlived their views; RemoveView did not run for them"),
			GStandaloneModels.Num());
		GStandaloneModels.Empty();
	}

	const int32 NumReleased = GKeyPaths.Num();
	GKeyPathLookup.Empty();
	GKeyPaths.Empty();

	// The one-shot diagnostics re-arm with the pool: a new UI thread is a new process's worth of
	// documents as far as the author is concerned, and a suite that boots several would
	// otherwise report the first mistake only.
	bWarnedBareRoot = false;
	bWarnedWrite = false;

	return NumReleased;
}
