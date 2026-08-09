// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FVaCuusUIThread;

/** One requested face: a VFS path, and whether it joins the fallback chain. */
struct FVaCuusFontFaceRequest
{
	/** Resolved by FVaCuusFileInterface against the ordered DevUI roots, like every other UI file. */
	FString VfsPath;

	/**
	 * A fallback face is consulted, IN REGISTRATION ORDER, for any character the styled face has
	 * no glyph for (FontFaceHandleDefault.cpp:367-383). That ordering is why this registry keeps
	 * an ordered array rather than a set.
	 */
	bool bFallbackFace = false;

	bool operator==(const FVaCuusFontFaceRequest& Other) const
	{
		return bFallbackFace == Other.bFallbackFace && VfsPath.Equals(Other.VfsPath, ESearchCase::CaseSensitive);
	}
};

/**
 * The runtime font-face door (spec 2026-08-09 §3), and the reason it has to be plugin-side:
 * Rml::LoadFontFace touches process-global font-provider state owned by the UI thread, and
 * FVaCuusUIThread::Enqueue is private, so no project can reach it.
 *
 * WHY THIS IS NEEDED AT ALL. The plugin ships fonts/LatoLatin-Regular.ttf and nothing else, and
 * that face covers Latin only -- measured: 20-7e, a0-17f plus symbols. No Cyrillic, no CJK. A
 * game that switches to Russian or Chinese without bringing its own face renders U+FFFD for
 * every character, and RmlUi substitutes that replacement silently
 * (FontFaceHandleDefault.cpp:386-393). RCSS `@font-face` is still the authoring route; this is
 * the runtime one, for a game that decides which face to load only once it knows the language.
 *
 * IT IS A REGISTRY AND NOT JUST A CALL, for the reason the style and translation registries are:
 * the UI thread can stop and start again within one process, and the RmlUi library goes down
 * with it. Without a replay, a restarted thread comes back with the shipped Latin face alone and
 * the game's own faces silently gone. PublishToUIThread is that replay, and it preserves
 * registration order because the fallback chain depends on it.
 *
 * Requests are deduplicated by (path, fallback flag): registering the same face twice is a
 * no-op, so a game may call this from a language-change handler without accumulating.
 */
class VACUUS_API FVaCuusFontRegistry
{
public:
	/**
	 * GAME THREAD. Records the request and, if a UI thread is up, enqueues the load. A face
	 * registered before any thread exists is loaded by PublishToUIThread at boot.
	 *
	 * REGISTER BEFORE LOADING THE DOCUMENTS THAT NEED IT: RmlUi resolves a font family when an
	 * element's text is laid out, and while it does re-resolve on restyle, a face that arrives
	 * late shows a frame of missing glyphs at best.
	 *
	 * @return false, with a Warning, for an empty path or a request already registered.
	 */
	static bool RegisterFace(const FString& VfsPath, bool bFallbackFace);

	/** GAME THREAD. Every request, in registration order — which is fallback order. */
	static const TArray<FVaCuusFontFaceRequest>& GetRequests_GameThread();

	/**
	 * GAME THREAD. Re-enqueues every request, in order, to a (re)started UI thread. Called from
	 * GetOrStartUIThread beside the style and translation registries' own replays; the queue is
	 * FIFO from this one producer, so faces enqueued here drain ahead of any document queued
	 * after them.
	 */
	static void PublishToUIThread(FVaCuusUIThread& UIThread);

	/**
	 * UI THREAD. Performs one load through Rml::LoadFontFace — the drain's handler for a
	 * LoadFontFace command. Failure is a Warning and not fatal: a missing face degrades to
	 * missing glyphs, and a boot that died over a font would be a worse trade.
	 */
	static void LoadFace_UIThread(const FString& VfsPath, bool bFallbackFace);

	/** UI THREAD. How many faces this thread has successfully loaded. The replay's observable. */
	static int32 GetNumFacesLoaded_UIThread();

	/**
	 * UI THREAD. Zeroes the loaded counter, called from FVaCuusUIThread::Init() — the count is
	 * per thread lifetime, because that is the thing a replay has to restore. The REQUEST list
	 * is game-thread state and deliberately survives, since it is what gets replayed.
	 */
	static void ResetLoadedCount_UIThread();
};
