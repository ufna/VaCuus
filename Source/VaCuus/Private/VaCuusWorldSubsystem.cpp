// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusWorldSubsystem.h"

#include "VaCuusDefines.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"

bool UVaCuusWorldSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	// Editor preview and inactive worlds have nothing to do with a game session
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld();
}

void UVaCuusWorldSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	UE_LOG(LogVaCuus, Log, TEXT("VaCuus world subsystem active (world: %s)"), *InWorld.GetName());
}

void UVaCuusWorldSubsystem::RegisterWorldComponent(UPrimitiveComponent* Component)
{
	check(IsInGameThread());
	if (Component == nullptr)
	{
		return;
	}

	WorldComponents.AddUnique(Component);
	UE_LOG(LogVaCuus, Verbose, TEXT("World panel '%s' registered (%d live)"),
		*Component->GetName(), WorldComponents.Num());
}

void UVaCuusWorldSubsystem::UnregisterWorldComponent(UPrimitiveComponent* Component)
{
	check(IsInGameThread());

	// One pass drops both this component and anything GC already invalidated, so the
	// roster never accumulates dead weak ptrs across level streaming.
	WorldComponents.RemoveAll([Component](const TWeakObjectPtr<UPrimitiveComponent>& Entry)
		{ return !Entry.IsValid() || Entry.Get() == Component; });
	UE_LOG(LogVaCuus, Verbose, TEXT("World panel '%s' unregistered (%d live)"),
		Component ? *Component->GetName() : TEXT("<null>"), WorldComponents.Num());
}
