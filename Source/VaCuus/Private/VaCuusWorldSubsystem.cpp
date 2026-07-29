// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusWorldSubsystem.h"

#include "VaCuusDefines.h"

#include "Engine/Engine.h"
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

	const FString Message = FString::Printf(TEXT("Hello world from VaCuus! (world: %s)"), *InWorld.GetName());

	UE_LOG(LogVaCuus, Log, TEXT("%s"), *Message);

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(INDEX_NONE, 10.f, FColor::Cyan, Message);
	}
}
