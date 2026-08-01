// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Templates/SharedPointer.h"

class FVaCuusUIThread;

/**
 * One immutable published translation table: key -> translated string, plus the
 * version the UI thread asserts monotonic. PUBLISH-BY-REPLACEMENT, the style-set
 * snapshot's discipline verbatim (FVaCuusStyleSnapshot): a table change never
 * mutates a published snapshot, it mints a whole new one with a higher Version.
 */
struct FVaCuusTranslationSnapshot
{
	/** Strictly increasing across the process; 0 never leaves the registry. */
	uint64 Version = 0;

	/** Translation key -> translated text. Keys are matched verbatim (case-sensitive). */
	TMap<FString, FString> Table;
};

/**
 * The process-wide localization registry (M5 Task 8, spec §2(l)) — the seam behind
 * `vacuus.translate(key, params?)` and the RML-text `TranslateString` path.
 *
 * WHY A SNAPSHOT AND NOT A PER-CALL HANDLER, stated because the obvious design is
 * wrong: `vacuus.translate` is called DURING JS execution on the UI thread and must
 * answer synchronously — a game-registered per-call callback would either run game
 * code on the UI thread (the one thing the threading model forbids) or turn a
 * synchronous JS API into an async round-trip that cannot return its own value.
 * So the handler seam is inverted: the game's "handler" is *push a new table*
 * (UVaCuusSubsystem::SetTranslationTable), and the UI thread answers every call
 * from the last installed immutable snapshot — the FVaCuusStyleRegistry shape,
 * minus the render-thread mirror it does not need.
 *
 *  - GAME THREAD (table-change rate): SetTable copies the map into a fresh
 *    snapshot, ++Version, and publishes through the existing command queue —
 *    never a lock.
 *  - UI-FRAME THREAD (per translate / per parsed text run): a pure TMap lookup
 *    in the installed snapshot.
 *
 * PROCESS-WIDE like the style registry (one RmlUi, one SystemInterface — RmlUi's
 * TranslateString has no per-context identity, SystemInterface.h:30), so
 * multi-PIE instances share one table; UVaCuusSubsystem::SetTranslationTable is
 * the per-instance door to the same registry.
 *
 * All state lives in the cpp; this class is only the door.
 */
class VACUUS_API FVaCuusTranslationRegistry
{
public:
	/**
	 * GAME THREAD. Replaces the whole table (publish-by-replacement — there is no
	 * incremental add; the producer owns the full table and pushes it whole, which is
	 * what makes the snapshot immutable by construction). Publishes to a running UI
	 * thread via the command queue; a thread started later gets it from
	 * PublishToUIThread in GetOrStartUIThread, exactly like the style snapshot.
	 *
	 * ALREADY-LOADED TEXT DOES NOT RE-TRANSLATE: RmlUi runs TranslateString at text
	 * instancing (Factory.cpp:336), so a table pushed after a document loaded reaches
	 * new text and `vacuus.translate` calls only. Push the table before LoadDocument,
	 * or reload the document after a language change — the FText re-push contract on
	 * UVaCuusView::UpdateModel is the same shape.
	 */
	static void SetTable(const TMap<FString, FString>& Table);

	/** GAME THREAD. Current version; 0 = no table ever published. */
	static uint64 GetVersion_GameThread();

	/** GAME THREAD. The current immutable snapshot; null before the first SetTable. */
	static TSharedPtr<const FVaCuusTranslationSnapshot> GetSnapshot_GameThread();

	/**
	 * GAME THREAD. Re-enqueues the current snapshot to a (re)started UI thread — the
	 * register-before-boot half: a table pushed before the first view must reach the
	 * first document's TranslateString, and the queue is FIFO from this one producer,
	 * so a snapshot enqueued before AddView/Load drains before them.
	 */
	static void PublishToUIThread(FVaCuusUIThread& UIThread);

	/**
	 * THE UI-FRAME-THREAD INSTALL — the drain's handler for a SetTranslationSnapshot
	 * command, and the test seam for contexts driven on the test thread. Owned by
	 * whichever single thread drives RmlUi (the owner-thread contract,
	 * FVaCuusStyleRegistry::InstallSnapshot's wording applies verbatim); checkf's the
	 * version monotonic — non-decreasing, because a re-publish of the same snapshot to
	 * a restarted UI thread is idempotent and a REGRESSION is the immutability bug.
	 */
	static void InstallSnapshot(const TSharedPtr<const FVaCuusTranslationSnapshot>& Snapshot);

	/** The installed snapshot, same thread contract as InstallSnapshot. Null = none yet. */
	static TSharedPtr<const FVaCuusTranslationSnapshot> GetInstalledSnapshot();

	/**
	 * UI-frame thread: Key through the installed snapshot. True = translated (Out
	 * written); false = identity — no table installed, or the key is not in it.
	 * DELIBERATELY QUIET on both misses: this runs per parsed text chunk
	 * (Factory.cpp:336 calls TranslateString for every text node in every document)
	 * and per translate() call; the one named refusal — no table has ever been
	 * pushed — is the JS thunk's per-context latched Verbose (spec §2(l)), where a
	 * caller actually asked for localization and got identity.
	 */
	static bool TranslateKey(const FString& Key, FString& Out);
};
