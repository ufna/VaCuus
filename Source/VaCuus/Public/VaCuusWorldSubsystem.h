// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Subsystems/WorldSubsystem.h"

#include "VaCuusWorldSubsystem.generated.h"

/**
 * Placeholder subsystem that announces itself when a game session starts.
 * Exists only to prove the plugin is loaded and running; replace once there is real functionality.
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
};
