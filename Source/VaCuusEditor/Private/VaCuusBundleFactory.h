// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Factories/Factory.h"

#include "VaCuusBundleFactory.generated.h"

/**
 * Creates UVaCuusBundle assets from the Content Browser (M6 Track B). Minimal on
 * purpose: the asset an editor save carries is a MARKER -- SourceNote and nothing
 * else -- because the loose DevUI tree stays authoritative and the pack happens
 * inside the asset's own PreSave when cooking (VaCuusBundle.h has the whole
 * argument). So there is nothing to configure here: the factory stamps provenance
 * and hands the empty shell over.
 *
 * The headless twin is `vacuus.Bundle.CreateAsset` (this file's .cpp): the machine
 * this plugin is developed on drives everything through -ExecCmds, and the cook
 * experiments need an asset without a Content Browser session.
 */
UCLASS(hidecategories = Object)
class UVaCuusBundleFactory : public UFactory
{
	GENERATED_BODY()

public:
	UVaCuusBundleFactory();

	//~ Begin UFactory
	virtual UObject* FactoryCreateNew(
		UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn) override;
	//~ End UFactory
};
