// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaCuusVectorResources.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/SystemInterface.h>
#include <lunasvg.h>

#include <limits>
#include <utility>

namespace VaCuusVectorResources
{
static thread_local std::string GSource;

FSourceScope::FSourceScope(const std::string& Source) : Previous(std::move(GSource)) { GSource = Source; }
FSourceScope::~FSourceScope() { GSource = std::move(Previous); }

bool LoadFile(const std::string& Path, std::string& Data)
{
	Rml::String Bytes;
	if (!Rml::GetFileInterface()->LoadFile(Path.c_str(), Bytes) || Bytes.size() > size_t(std::numeric_limits<int>::max()))
	{
		return false;
	}
	Data.assign(Bytes.data(), Bytes.size());
	return true;
}

bool LoadRelativeFile(const std::string& Path, std::string& Data)
{
	Rml::String Joined;
	Rml::GetSystemInterface()->JoinPath(Joined, GSource.c_str(), Path.c_str());
	return LoadFile(Joined.c_str(), Data);
}

bool LoadChildFile(const std::string& Directory, const std::string& Path, std::string& Data)
{
	// rlottie's host realpath check cannot resolve bundle entries. Keep asset paths inside the animation directory.
	if (Path.empty() || Path.front() == '/' || Path.front() == '\\' || Path.find(':') != std::string::npos ||
		Path.find('\0') != std::string::npos) return false;
	int Depth = 0;
	for (size_t Start = 0; Start < Path.size();)
	{
		const size_t End = Path.find_first_of("/\\", Start);
		const std::string Part = Path.substr(Start, End == std::string::npos ? End : End - Start);
		if (Part == "..") { if (--Depth < 0) return false; }
		else if (!Part.empty() && Part != ".") ++Depth;
		if (End == std::string::npos) break;
		Start = End + 1;
	}
	Rml::String Joined;
	Rml::GetSystemInterface()->JoinPath(Joined, (Directory + "__resource_base__").c_str(), Path.c_str());
	return LoadFile(Joined.c_str(), Data);
}

void InstallDefaultFonts()
{
	// Use the shipped face instead of scanning OS font directories, which differ on packaged targets.
	for (const char* Family : {"LatoLatin", "sans-serif", "serif"})
	{
		auto* Bytes = new std::string;
		if (!LoadFile("fonts/LatoLatin-Regular.ttf", *Bytes))
		{
			delete Bytes;
			continue;
		}
		// PlutoVG consumes the callback even if the font is rejected (plutovg-font.c:302-308).
		lunasvg_add_font_face_from_data(Family, false, false, Bytes->data(), Bytes->size(),
			[](void* Value) { delete static_cast<std::string*>(Value); }, Bytes);
	}
}
}
