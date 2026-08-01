// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include <RmlUi/Core/FileInterface.h>

#include <atomic>

/**
 * Serves RmlUi file requests -- bundle-first, then the UE platform file layer.
 *
 * A relative path probes the mounted bundles (FVaCuusBundleMountTable, in mount
 * order, first hit wins) BEFORE the loose roots: the bundle exists to make shipping
 * deterministic, and a stale loose file must not shadow it in a packaged Development
 * build (spec M6 2(d)). A miss with bundles mounted logs a Warning naming every
 * probed bundle -- the silent-miss killer -- then falls back to the ordered DevUI
 * roots (plugin first, then project; VaCuusContentPaths.h, controller decision D19).
 * Absolute paths bypass bundles and pass through unchanged.
 *
 * The Rml::FileHandle values handed out are NOT IFileHandle pointers: they address a small
 * record holding either a bundle span or the handle plus the LOGICAL read position,
 * because IFileHandle cannot represent a position at exact EOF. See FOpenFile in the .cpp.
 */
class FVaCuusFileInterface : public Rml::FileInterface
{
public:
	/**
	 * The serving totals print at teardown: with a bundle mounted, every loose-served
	 * open is a potential stale-shadow bug, and the packaged bundle gates assert the
	 * loose count is ZERO (spec M6 2(d)'s M==0). Logged from the destructor because
	 * this object's life IS the RmlUi session (FVaCuusEngine owns it boot-to-shutdown).
	 */
	virtual ~FVaCuusFileInterface() override;

	//~ Begin Rml::FileInterface
	virtual Rml::FileHandle Open(const Rml::String& Path) override;
	virtual void Close(Rml::FileHandle File) override;
	virtual size_t Read(void* Buffer, size_t Size, Rml::FileHandle File) override;
	virtual bool Seek(Rml::FileHandle File, long Offset, int Origin) override;
	virtual size_t Tell(Rml::FileHandle File) override;
	virtual size_t Length(Rml::FileHandle File) override;
	//~ End Rml::FileInterface

	/** Opens served from a mounted bundle over this interface's life. Any thread. */
	uint64 GetNumBundleOpens() const { return NumBundleOpens.load(std::memory_order_relaxed); }

	/** Opens served from the loose roots (or absolute passthrough). Any thread. */
	uint64 GetNumLooseOpens() const { return NumLooseOpens.load(std::memory_order_relaxed); }

private:
	std::atomic<uint64> NumBundleOpens{0};
	std::atomic<uint64> NumLooseOpens{0};
};
