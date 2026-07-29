// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include <RmlUi/Core/SystemInterface.h>

/** Routes RmlUi time queries and log output to UE (FPlatformTime / LogVaCuus). */
class FVaCuusSystemInterface : public Rml::SystemInterface
{
public:
	FVaCuusSystemInterface();

	//~ Begin Rml::SystemInterface
	virtual double GetElapsedTime() override;
	virtual bool LogMessage(Rml::Log::Type Type, const Rml::String& Message) override;
	//~ End Rml::SystemInterface

private:
	/** FPlatformTime::Seconds() at construction; GetElapsedTime is relative to this. */
	double StartTime = 0.0;
};
