// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Kismet2/StructureEditorUtils.h"

/**
 * The editor half of the Blueprint-struct-recompile refusal (VaCuus-akj.16, spec M6 2(j)).
 *
 * WHY IT EXISTS: a designer editing a Blueprint struct WHILE a model bound over it is live is
 * a normal editor action, and before this guard it was silent heap corruption -- the compile
 * `delete`s every FProperty of the type (CleanAndSanitizeStruct ->
 * DestroyChildPropertiesAndResetPropertyLinks, UserDefinedStructureCompilerUtils.cpp:225)
 * while FVaCuusModelLayout keeps raw pointers to them and FVaCuusModelShadow's destructor
 * walks the NEW DestructorLink over OLD-layout buffers. VaCuus.Model.BlueprintRecompile
 * records both facts; this class is the refusal built on them.
 *
 * WHY IT LIVES IN VaCuusEditor: FStructureEditorUtils is UnrealEd, which only an Editor
 * module may depend on. Everything that has to HAPPEN lives in the runtime module behind
 * UVaCuusSubsystem::NotifyStructPreRecompile (the walk, the condemnation, the fenced UI-side
 * teardown); this class is one forwarding call and the registration plumbing.
 *
 * THE TIMING IS THE LOAD-BEARING FACT, verified in source: BroadcastPreChange fires at
 * UserDefinedStructureCompilerUtils.cpp:599, BEFORE the compile at :622 -- so inside
 * PreChange the OLD property chain is still alive, which is the one moment a synchronous
 * DestroyStruct teardown of old-layout buffers is safe. That window is exactly what the
 * runtime side's fence spends.
 *
 * REGISTRATION IS THE CONSTRUCTOR, BY THE ENGINE'S OWN SHAPE: INotifyOnStructChanged is
 * FStructEditorManager::ListenerType (StructureEditorUtils.h:28-42), whose InnerListenerType
 * base AddListener()s itself in its constructor and RemoveListener()s in its destructor
 * (ListenerManager.h:25-32). Owning an instance IS being subscribed, which is why the editor
 * module holds one by TUniquePtr for exactly its own lifetime.
 */
class FVaCuusStructRecompileGuard final : public FStructureEditorUtils::INotifyOnStructChanged
{
public:
	//~ Begin INotifyOnStructChanged
	virtual void PreChange(const UUserDefinedStruct* Changed, FStructureEditorUtils::EStructureEditorChangeInfo ChangedType) override;
	virtual void PostChange(const UUserDefinedStruct* Changed, FStructureEditorUtils::EStructureEditorChangeInfo ChangedType) override;
	//~ End INotifyOnStructChanged
};
