// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Subsystems/WorldSubsystem.h"

#include "VaCuusWorldSubsystem.generated.h"

class UPrimitiveComponent;

/**
 * Per-world roster of live VaCuus world-space panels (M5 Task 6).
 *
 * UVaCuusWorldComponent registers here from OnRegister/OnUnregister; the M5 Task 7
 * input processor hangs off these calls -- first registration installs the
 * process-wide IInputProcessor, last removal uninstalls it (the refcount idiom of
 * UWidgetComponent's hit-tester registration, WidgetComponent.cpp:1097-1138) -- and
 * walks GetWorldComponents() to resolve a trace hit to a panel.
 *
 * TYPED AS UPrimitiveComponent, NOT UVaCuusWorldComponent, and that is module
 * layering rather than sloppiness: the component lives in VaCuusRender (it needs the
 * replayer and the frame sink, both private there), VaCuusRender depends on VaCuus,
 * and this header cannot name a type from a module that depends on it. Callers on
 * the VaCuusRender side cast.
 */
UCLASS()
class VACUUS_API UVaCuusWorldSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin USubsystem
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	//~ End USubsystem

	//~ Begin UWorldSubsystem
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	//~ End UWorldSubsystem

	/** Adds a panel to the roster. Game thread; idempotent per component. */
	void RegisterWorldComponent(UPrimitiveComponent* Component);

	/** Removes a panel; also drops any entries GC beat us to. Game thread. */
	void UnregisterWorldComponent(UPrimitiveComponent* Component);

	/** The live roster. Entries can be stale between a GC and the owner's unregister. */
	const TArray<TWeakObjectPtr<UPrimitiveComponent>>& GetWorldComponents() const { return WorldComponents; }

private:
	/** See GetWorldComponents(). */
	TArray<TWeakObjectPtr<UPrimitiveComponent>> WorldComponents;
};
