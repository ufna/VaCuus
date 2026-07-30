// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include <RmlUi/Core/SystemInterface.h>

/**
 * Routes RmlUi time queries, log output and cursor requests to UE
 * (FPlatformTime / LogVaCuus / EMouseCursor).
 */
class FVaCuusSystemInterface : public Rml::SystemInterface
{
public:
	FVaCuusSystemInterface();

	//~ Begin Rml::SystemInterface
	virtual double GetElapsedTime() override;
	virtual bool LogMessage(Rml::Log::Type Type, const Rml::String& Message) override;

	/**
	 * Cursor shape is PUSH-based, and this is the only place it can be caught.
	 * RmlUi calls this from inside Context::Update's hover-chain pass and ONLY when
	 * the name changed (Context.cpp:1315-1327); there is no "what cursor do you want"
	 * query to answer from the game thread. So the name is latched here and each
	 * host picks it up right after its own Update() -- see
	 * GetVaCuusLatchedMouseCursor() for why that attributes the change to the right
	 * view.
	 */
	virtual void SetMouseCursor(const Rml::String& CursorName) override;
	//~ End Rml::SystemInterface

private:
	/** FPlatformTime::Seconds() at construction; GetElapsedTime is relative to this. */
	double StartTime = 0.0;
};
