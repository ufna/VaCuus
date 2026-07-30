// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include <RmlUi/Core/FileInterface.h>

/**
 * Serves RmlUi file requests through the UE platform file layer.
 *
 * Relative paths are resolved against the ordered DevUI roots -- the plugin's
 * Content/DevUI first, then the project's (see VaCuusContentPaths.h and controller
 * decision D19); absolute paths are passed through unchanged.
 *
 * The Rml::FileHandle values handed out are NOT IFileHandle pointers: they address a small
 * record holding the handle plus the LOGICAL read position, because IFileHandle cannot
 * represent a position at exact EOF. See FOpenFile in the .cpp.
 */
class FVaCuusFileInterface : public Rml::FileInterface
{
public:
	//~ Begin Rml::FileInterface
	virtual Rml::FileHandle Open(const Rml::String& Path) override;
	virtual void Close(Rml::FileHandle File) override;
	virtual size_t Read(void* Buffer, size_t Size, Rml::FileHandle File) override;
	virtual bool Seek(Rml::FileHandle File, long Offset, int Origin) override;
	virtual size_t Tell(Rml::FileHandle File) override;
	virtual size_t Length(Rml::FileHandle File) override;
	//~ End Rml::FileInterface
};
