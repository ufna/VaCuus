// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusModelLayout.h"

#include "Kismet2/StructureEditorUtils.h"
#include "StructUtils/UserDefinedStruct.h"

// FStructVariableDescription is only FORWARD-declared by StructureEditorUtils.h:10, and
// GetVarDesc() hands back a TArray of them -- so indexing that array needs the definition,
// which lives here (UserDefinedStructEditorData.h:34).
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * SPEC 11's LAST **[unverified]** ROW, SETTLED: what happens to a live FVaCuusModelLayout when
 * the Blueprint struct under it is recompiled.
 *
 * The spec's mitigation reads "Strong ref + rebuild". THE FIRST HALF WORKS AND THE SECOND HALF
 * DOES NOT EXIST, and this test is the evidence for both halves at once.
 *
 * WHAT A RECOMPILE ACTUALLY DOES, read rather than assumed. Every structural edit funnels
 * through FStructureEditorUtils::OnStructureChanged, which sets Status = UDSS_Dirty and calls
 * CompileStructure (StructureEditorUtils.cpp:569-580). That reaches
 * FUserDefinedStructureCompilerUtils::CompileStruct, whose own comment says "Recompiled UDS keep
 * the same instance" (UserDefinedStructureCompilerUtils.cpp:595) -- so the UObject survives,
 * and a TStrongObjectPtr to it stays valid. But the first thing done to that surviving instance
 * is CleanAndSanitizeStruct, which calls DestroyChildPropertiesAndResetPropertyLinks
 * (:512, :225) -> DestroyPropertyLinkedList, which is a plain `delete FieldToDestroy` over the
 * whole chain (Class.cpp:2117-2126). Every FProperty is FREED and a fresh set is built by
 * CreateVariables (:244).
 *
 * FVaCuusModelLayout holds raw `const FProperty*` per field (VaCuusModelLayout.h's
 * FVaCuusModelField::Property), justified there by "an already-linked type's FProperty chain is
 * not rewritten at runtime". That is true of a NATIVE type and false of a UUserDefinedStruct in
 * the editor, which is exactly the case the strong reference was added for.
 *
 * THE CONSEQUENCES, IN ORDER OF SEVERITY, none of which this test provokes:
 *
 *  1. FVaCuusModelShadow's destructor calls Struct->DestroyStruct(Data) on a buffer laid out by
 *     the OLD properties using the NEW DestructorLink. For any heap-owning member that is a free
 *     of a pointer read from the wrong offset. This is the one that corrupts the heap, and it is
 *     why the test below builds a LAYOUT and never a shadow.
 *  2. Sample(), Publish() and ApplyUpdate() all dereference Fields[i].Property, i.e. freed
 *     memory, every frame.
 *  3. RmlUi keeps a raw void* to the UI shadow's base and revalidates it never, so even a
 *     correct rebuild would have to happen on the UI thread with the context still up.
 *
 * SCOPE, AND WHY THIS IS NOT A MERGE BLOCKER ON ITS OWN: UUserDefinedStruct recompilation is
 * editor-only -- FStructureEditorUtils lives in UnrealEd and nothing in a packaged game can
 * reach it -- so a shipped title is unaffected. In PIE it needs a designer to edit a Blueprint
 * struct WHILE a view bound over that struct is live, which the editor does not do implicitly.
 *
 * WHAT THIS TEST ASSERTS is the pair of facts a fix would have to be built on, so that it fails
 * loudly if either changes: the type object survives, and the properties do not. It deliberately
 * does not assert "the layout is broken", because the day somebody adds FStructEditorManager
 * listening and a rebuild, that assertion would be the thing standing in the way.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusModelBlueprintRecompileTest, "VaCuus.Model.BlueprintRecompile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusModelBlueprintRecompileTest::RunTest(const FString& Parameters)
{
	if (!FStructureEditorUtils::UserDefinedStructEnabled())
	{
		// [UserDefinedStructure] bUseUserDefinedStructure gates the whole feature, and
		// CreateUserDefinedStruct answers null when it is off (StructureEditorUtils.cpp:42-64).
		AddInfo(TEXT("Skipped: user-defined structs are disabled in this configuration"));
		return true;
	}

	TStrongObjectPtr<UUserDefinedStruct> Struct(
		FStructureEditorUtils::CreateUserDefinedStruct(GetTransientPackage(), TEXT("VaCuusRecompileProbe"), RF_Transient));
	if (!TestTrue(TEXT("a Blueprint struct was created"), Struct.IsValid()))
	{
		return false;
	}

	// CreateUserDefinedStruct adds one bool member and compiles (StructureEditorUtils.cpp:58-60),
	// so there is something to bind before anything is edited.
	const FVaCuusModelLayout Layout(Struct.Get());
	if (!TestTrue(TEXT("the layout resolved the Blueprint struct"), Layout.IsValid()))
	{
		return false;
	}
	if (!TestEqual(TEXT("with the struct's one default member bound"), Layout.GetFields().Num(), 1))
	{
		return false;
	}

	const UScriptStruct* TypeBefore = Layout.GetStruct();
	const FProperty* PropertyBefore = Layout.GetFields()[0].Property;
	const int32 SizeBefore = Struct->GetStructureSize();
	const FString WireNameBefore = Layout.GetFields()[0].WireName;

	TArray<FStructVariableDescription>& VarDescs = FStructureEditorUtils::GetVarDesc(Struct.Get());
	if (!TestEqual(TEXT("the struct has one variable description"), VarDescs.Num(), 1))
	{
		return false;
	}
	const FGuid VarGuid = VarDescs[0].VarGuid;

	// ---- THE RECOMPILE. ----
	//
	// RenameVariable rather than AddVariable, only because it needs no FEdGraphPinType and
	// therefore no BlueprintGraph dependency in this module. It reaches the same door:
	// OnStructureChanged(RenamedVariable) -> Status = UDSS_Dirty -> CompileStructure
	// (StructureEditorUtils.cpp:347-373, :569-580). The change reason matters -- and this one is
	// not the exempt one: CleanAndSanitizeStruct skips the property teardown ONLY for
	// DefaultValueChanged (UserDefinedStructureCompilerUtils.cpp:221-232).
	if (!TestTrue(TEXT("the variable was renamed, which recompiles the struct"),
			FStructureEditorUtils::RenameVariable(Struct.Get(), VarGuid, TEXT("RenamedByTheTest"))))
	{
		return false;
	}

	// ---- FACT 1: THE TYPE OBJECT SURVIVES. ----
	//
	// So FVaCuusModelLayout's and FVaCuusModelShadow's TStrongObjectPtr are pointing at a live,
	// valid UScriptStruct -- the half of the spec's mitigation that does work. Nothing here is
	// protected by that; it is what keeps the failure from being a null dereference.
	TestTrue(TEXT("the recompile kept the same UScriptStruct instance"), Layout.GetStruct() == TypeBefore);
	TestTrue(TEXT("and it is still alive"), Struct.IsValid());
	TestEqual(TEXT("and the layout still names it"), Layout.GetStruct(), static_cast<const UScriptStruct*>(Struct.Get()));

	// ---- FACT 2: EVERY FProperty THE LAYOUT RESOLVED IS GONE. ----
	//
	// Compared by POINTER VALUE ONLY. PropertyBefore has been `delete`d, so dereferencing it --
	// including calling GetName() on it for a nicer message -- is a use-after-free. The whole
	// question is whether the address the layout is still holding is one of the struct's
	// properties, and address comparison answers exactly that.
	bool bOldPropertyStillPresent = false;
	int32 NumPropertiesAfter = 0;
	for (const FProperty* Property = Struct->PropertyLink; Property != nullptr; Property = Property->PropertyLinkNext)
	{
		++NumPropertiesAfter;
		bOldPropertyStillPresent |= (Property == PropertyBefore);
	}

	TestEqual(TEXT("the struct still has one property afterwards"), NumPropertiesAfter, 1);
	TestFalse(TEXT("but it is NOT the one the layout resolved -- the layout's FProperty* is dangling"),
		bOldPropertyStillPresent);

	// The layout is now describing a type it no longer matches, and says nothing about it. Both
	// of these read fine because they are the layout's own copies; that is the point.
	TestEqual(TEXT("the layout's cached wire name is the OLD one"), Layout.GetFields()[0].WireName, WireNameBefore);
	TestEqual(TEXT("...while the struct now authors it differently"),
		FStructureEditorUtils::GetVariableFriendlyNameForProperty(Struct.Get(), Struct->PropertyLink),
		FString(TEXT("RenamedByTheTest")));

	AddInfo(FString::Printf(
		TEXT("Blueprint struct recompile: type object %s (same instance), size %d -> %d, layout's FProperty* %s. ")
		TEXT("The layout holds a strong ref to the TYPE, which survives; it holds RAW pointers to the PROPERTIES, ")
		TEXT("which do not. There is no rebuild hook in M3a."),
		Layout.GetStruct() == TypeBefore ? TEXT("SURVIVED") : TEXT("REPLACED"), SizeBefore, Struct->GetStructureSize(),
		bOldPropertyStillPresent ? TEXT("still valid") : TEXT("DANGLING")));

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
