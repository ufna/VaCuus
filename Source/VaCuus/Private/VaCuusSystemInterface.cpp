// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusSystemInterface.h"

#include "VaCuusDefines.h"

#include "HAL/PlatformTime.h"

FVaCuusSystemInterface::FVaCuusSystemInterface()
	: StartTime(FPlatformTime::Seconds())
{
}

double FVaCuusSystemInterface::GetElapsedTime()
{
	return FPlatformTime::Seconds() - StartTime;
}

bool FVaCuusSystemInterface::LogMessage(Rml::Log::Type Type, const Rml::String& Message)
{
	const FString Text = UTF8_TO_TCHAR(Message.c_str());

	switch (Type)
	{
	case Rml::Log::LT_ERROR:
	case Rml::Log::LT_ASSERT:
		UE_LOG(LogVaCuus, Error, TEXT("[Rml] %s"), *Text);
		break;
	case Rml::Log::LT_WARNING:
		UE_LOG(LogVaCuus, Warning, TEXT("[Rml] %s"), *Text);
		break;
	case Rml::Log::LT_ALWAYS:
		UE_LOG(LogVaCuus, Display, TEXT("[Rml] %s"), *Text);
		break;
	case Rml::Log::LT_INFO:
		UE_LOG(LogVaCuus, Log, TEXT("[Rml] %s"), *Text);
		break;
	case Rml::Log::LT_DEBUG:
	default:
		UE_LOG(LogVaCuus, Verbose, TEXT("[Rml] %s"), *Text);
		break;
	}

	// Continue execution (returning false asks RmlUi to break into the debugger).
	return true;
}
