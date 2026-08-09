// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusFontRegistry.h"

#include "VaCuus.h"
#include "VaCuusContentPaths.h"
#include "VaCuusDefines.h"
#include "VaCuusUIThread.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/FontEngineInterface.h>

namespace
{
/** GAME-THREAD state, in registration order because that is fallback order. */
TArray<FVaCuusFontFaceRequest> GFontRequests;

/** UI-THREAD state: faces this thread's RmlUi has actually taken. */
int32 GNumFacesLoaded = 0;
}	 // namespace

bool FVaCuusFontRegistry::RegisterFace(const FString& VfsPath, bool bFallbackFace)
{
	check(IsInGameThread());

	if (VfsPath.IsEmpty())
	{
		UE_LOG(LogVaCuus, Warning, TEXT("RegisterFace with an empty path; nothing registered"));
		return false;
	}

	const FVaCuusFontFaceRequest Request{VfsPath, bFallbackFace};
	if (GFontRequests.Contains(Request))
	{
		// Deliberately quiet at Warning rather than Error: a game calling this from a
		// language-change handler will re-register the same face on every switch, and that is a
		// reasonable thing to write.
		UE_LOG(LogVaCuus, Verbose, TEXT("Font face '%s' (fallback=%d) is already registered; ignored"),
			*VfsPath, bFallbackFace ? 1 : 0);
		return false;
	}

	GFontRequests.Add(Request);

	if (FVaCuusModule* Module = FVaCuusModule::GetPtr())
	{
		if (FVaCuusUIThread* UIThread = Module->GetUIThread())
		{
			UIThread->EnqueueLoadFontFace(VfsPath, bFallbackFace);
		}
	}

	UE_LOG(LogVaCuus, Log, TEXT("Font face registered: '%s' (fallback=%d, %d total)"),
		*VfsPath, bFallbackFace ? 1 : 0, GFontRequests.Num());
	return true;
}

const TArray<FVaCuusFontFaceRequest>& FVaCuusFontRegistry::GetRequests_GameThread()
{
	check(IsInGameThread());
	return GFontRequests;
}

void FVaCuusFontRegistry::PublishToUIThread(FVaCuusUIThread& UIThread)
{
	check(IsInGameThread());

	// IN ORDER, and the order is load-bearing: RmlUi consults fallback faces by their position
	// in the provider's list (FontFaceHandleDefault.cpp:367-383), so a replay that reordered
	// them would change which face wins a glyph after a UI-thread restart.
	for (const FVaCuusFontFaceRequest& Request : GFontRequests)
	{
		UIThread.EnqueueLoadFontFace(Request.VfsPath, Request.bFallbackFace);
	}
}

void FVaCuusFontRegistry::LoadFace_UIThread(const FString& VfsPath, bool bFallbackFace)
{
	check(FVaCuusUIThread::IsInUIThread());

	// THE EXISTENCE CHECK GOES THROUGH THE SAME ORDERED ROOTS the file interface uses, exactly as
	// the default face's load does (VaCuusEngine.cpp:138-144), so this cannot disagree with
	// LoadFontFace below about which copy exists -- and it is what turns "the file is not there"
	// into a message naming the roots that were searched.
	const FString DiskPath = VaCuusContentPaths::ResolveExistingDocument(*VfsPath);
	if (DiskPath.IsEmpty())
	{
		UE_LOG(LogVaCuus, Warning,
			TEXT("Font face '%s' not found under any DevUI root (%s); text needing it renders the replacement character"),
			*VfsPath, *FString::Join(VaCuusContentPaths::GetDocumentRoots(), TEXT(" | ")));
		return;
	}

	// Relative path on purpose: FVaCuusFileInterface resolves it, so a bundled face works
	// identically to a loose one.
	if (!Rml::LoadFontFace(TCHAR_TO_UTF8(*VfsPath), bFallbackFace))
	{
		// NOT FATAL. A face that FreeType refuses costs missing glyphs; a boot that died over a
		// font would cost the whole UI, which is the worse trade.
		UE_LOG(LogVaCuus, Warning, TEXT("RmlUi refused the font face at '%s' (fallback=%d)"), *DiskPath, bFallbackFace ? 1 : 0);
		return;
	}

	++GNumFacesLoaded;
	UE_LOG(LogVaCuus, Log, TEXT("Font face loaded on the UI thread: '%s' (fallback=%d, %d this thread)"),
		*VfsPath, bFallbackFace ? 1 : 0, GNumFacesLoaded);
}

int32 FVaCuusFontRegistry::GetNumFacesLoaded_UIThread()
{
	check(FVaCuusUIThread::IsInUIThread());
	return GNumFacesLoaded;
}

int32 FVaCuusFontRegistry::GetNumReplacementGlyphs_UIThread()
{
	check(FVaCuusUIThread::IsInUIThread());

	// Through the interface rather than the default engine directly, because that is the seam a
	// game replacing the font engine would substitute at -- and the base implementation returns 0,
	// so a custom engine reports "cannot say" rather than failing to compile.
	Rml::FontEngineInterface* FontEngine = Rml::GetFontEngineInterface();
	return FontEngine != nullptr ? FontEngine->GetNumReplacementGlyphs() : 0;
}

void FVaCuusFontRegistry::ResetLoadedCount_UIThread()
{
	check(FVaCuusUIThread::IsInUIThread());
	GNumFacesLoaded = 0;
}
