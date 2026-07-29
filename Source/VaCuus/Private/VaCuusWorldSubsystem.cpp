// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusWorldSubsystem.h"

#include "VaCuusDefines.h"

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
