// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include <string>

namespace VaCuusVectorResources
{
// SVG image hrefs have no base URI in LunaSVG; keep it scoped to the document being parsed.
class FSourceScope
{
public:
	explicit FSourceScope(const std::string& Source);
	~FSourceScope();
	FSourceScope(const FSourceScope&) = delete;
	FSourceScope& operator=(const FSourceScope&) = delete;
private:
	std::string Previous;
};

bool LoadFile(const std::string& Path, std::string& Data);
bool LoadRelativeFile(const std::string& Path, std::string& Data);
bool LoadChildFile(const std::string& Directory, const std::string& Path, std::string& Data);
void InstallDefaultFonts();
}
