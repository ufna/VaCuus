// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Templates/SharedPointer.h"

#include <type_traits> // std::is_same_v, for VACUUS_ASSERT_HOST_FORWARDS below

struct FVaCuusViewStatus;

namespace Rml
{
class Context;
}

/**
 * The UI thread's view of "the thing that owns ONE view's RmlUi context and
 * document". One host instance per UVaCuusView; the process-wide UI thread owns
 * the id -> host map and routes commands into it.
 *
 * WHY AN INTERFACE: the concrete host needs the recording render interface and
 * the Slate element, which live in VaCuusRender, and VaCuusRender already
 * depends on VaCuus -- a direct member would close the module cycle. So VaCuus
 * declares the contract and VaCuusRender implements it
 * (FVaCuusRmlDocumentHost). The seam is deliberate: it is also where a
 * non-RmlUi or headless host can plug in (the multi-view automation test does
 * exactly that).
 *
 * THREAD AFFINITY: everything from Initialize() onwards runs ON THE VaCuus UI
 * THREAD and nowhere else -- the UI thread boots the host when it drains the
 * AddView command and shuts it down on RemoveView or in Exit(), so RmlUi is only
 * ever touched from that one thread. Implementations assert this with
 * check(FVaCuusUIThread::IsInUIThread()). Construction is the single exception:
 * the owner builds the host on its own thread and hands it over with AddView.
 *
 * LIBRARY LIFETIME: the host does NOT boot or shut down RmlUi -- the UI thread
 * does that once for the process (FVaCuusEngine is a process-wide singleton
 * because RmlUi's interfaces, its `initialised` flag and its context registry
 * are process-global statics). A host only creates and destroys its own context.
 */
class IVaCuusDocumentHost
{
public:
	virtual ~IVaCuusDocumentHost() = default;

	/**
	 * Boots this view's RmlUi state (render interface, context) under InViewId.
	 * RmlUi is already initialized when this is called. InStatus is how load
	 * results and published-frame counts get back to the game thread.
	 *
	 * Must fully roll back and return false on failure: the UI thread drops a
	 * host whose Initialize() failed without calling Shutdown().
	 */
	virtual bool Initialize(uint32 InViewId, const TSharedRef<FVaCuusViewStatus>& InStatus) = 0;

	/**
	 * Idempotent teardown of everything Initialize() built, EXCEPT anything
	 * RmlUi's global state still points at -- for the RmlUi host that is the
	 * per-view render interface, because Rml::Shutdown() destroys the
	 * RenderManager it keyed on that pointer and releases resources through it.
	 * The UI thread therefore keeps a shut-down host alive (retired) and only
	 * destroys it after RmlUi itself is down.
	 */
	virtual void Shutdown() = 0;

	/** Sets the layout/record size in pixels. Idempotent; an unchanged size costs nothing. */
	virtual void SetViewSize(FIntPoint ViewSize) = 0;

	/** Replaces the current document with one loaded through the file interface. */
	virtual void LoadDocumentFromFile(const FString& VfsPath, uint64 LoadSerial) = 0;

	/** Replaces the current document with one parsed from RML source text. */
	virtual void LoadDocumentFromMemory(const FString& RmlSource, uint64 LoadSerial) = 0;

	/**
	 * Closes the current document, if any. The host stays initialized.
	 *
	 * A HOST THAT OWNS A RENDER TARGET OWES ONE MORE FRAME AFTER THIS. Since the M2 Task
	 * 12 idle gate the render target is the only copy of an idle UI's pixels -- withheld
	 * frames are never resent, and the Slate element composites the RT whether or not a
	 * buffer arrived -- so a host that let HasView() go false here would leave the closed
	 * document composited forever. FVaCuusRmlDocumentHost keeps HasView() true for exactly
	 * one post-close frame; the empty frame it records is what clears the RT, and its
	 * Update() is also what makes RmlUi actually free the document. A host with no render
	 * target of its own (the multi-view test's probe) owes nothing.
	 */
	virtual void CloseDocument() = 0;

	/**
	 * Shows or hides the current document.
	 *
	 * A hidden view keeps RECORDING frames, which is what clears it off the screen --
	 * skipping the frame instead would leave the last visible content in the view's RT,
	 * because nothing would ever emit the empty frame that clears it. It does NOT keep
	 * publishing them: the first hidden frame records an empty command list and publishes
	 * it, and every hidden frame after that hashes equal and is withheld by the idle gate.
	 * That is safe precisely because the RT was already cleared by the frame before. (This
	 * sentence used to claim the empty frames keep publishing; that has been false since M2
	 * Task 12 -- see FVaCuusRmlDocumentHost::SetVisible.)
	 */
	virtual void SetVisible(bool bVisible) = 0;

	/**
	 * True when a frame must be produced this tick: normally "a document is loaded and the
	 * view size is valid", plus the one post-close frame CloseDocument() owes.
	 */
	virtual bool HasView() const = 0;

	/**
	 * Takes delivery of whatever off-thread work has finished for this host. Called once per
	 * UI frame for EVERY live host, BEFORE the HasView() test above -- which is the whole
	 * point of it existing separately from RecordAndPublishFrame().
	 *
	 * WHY IT CANNOT LIVE INSIDE THE RECORD (bead VaCuus-akj.6.27): the async image decodes are
	 * drained by the recorder's BeginFrame(), so before this they were drained only on a
	 * RECORDABLE frame -- and HasView() is a recordability test, not a liveness test: it also
	 * demands a non-degenerate view size. The decode itself is launched from a size-INDEPENDENT
	 * path (AdoptDocument -> Document->Show() -> layout -> ElementImage::GetIntrinsicDimensions
	 * -> FileTextureDatabase::EnsureLoaded -> FVaCuusRecordingRenderInterface::LoadTexture), and
	 * the shipped UMG route drives exactly that mismatch on every auto-loading widget:
	 * UVaCuusWidget::RebuildWidget creates its view at FIntPoint::ZeroValue on purpose
	 * (VaCuusUMGWidget.cpp:75-76) and SynchronizeProperties loads the document immediately
	 * afterwards (:121-122), while the only correct size arrives on the first
	 * SVaCuusWidget::Tick (SVaCuusWidget.cpp:253-256). Finished payloads decoded in that window
	 * -- one measured at 144 MB for a single 6000x6000 PNG (bead akj.6.25) -- used to sit in the
	 * recorder's queue for the whole unsized window, which for a widget that is never arranged
	 * is the rest of the session.
	 *
	 * DEFAULT-EMPTY, the "a host with no render target of its own owes nothing" shape
	 * CloseDocument() uses: a host with no off-thread work has nothing to take delivery of, and
	 * the ~two dozen test probe hosts implement no decode path. A DECORATOR MUST STILL FORWARD
	 * IT: inheriting this default would silently reinstate the bug for the host it wraps, so
	 * FVaCuusLobbyDemoHost forwards it under a VACUUS_ASSERT_HOST_FORWARDS below, which fails
	 * the BUILD if that forward is ever dropped.
	 *
	 * UI thread only, like everything else from Initialize() onwards.
	 */
	virtual void DrainAsyncArrivals() {}

	/**
	 * Drops cached file textures for this view: one by VFS source, or ALL when Source is
	 * empty. Answers whether anything was actually released. UI thread (VaCuus-dqs.2).
	 *
	 * WHY THIS EXISTS AT ALL. RmlUi's FileTextureDatabase has NO eviction: an entry is
	 * cleared only by an explicit release, and closing a document or destroying the element
	 * does not do it (TextureDatabase.cpp:151-178, and the recorder's own note at
	 * VaCuusRecordingRenderInterface.cpp:1436). A catalogue screen therefore accumulates
	 * images for the life of the UI thread unless someone asks.
	 *
	 * IT IS SAFE ON A LIVE IMAGE, and that is what makes it usable rather than a footgun:
	 * ReleaseTexture assigns `texture = {}`, clearing the handle AND the load-failed flag, so
	 * the next EnsureLoaded re-runs LoadTextureEntry (TextureDatabase.cpp:117-129). Elements
	 * hold a TextureFileIndex rather than a handle, so nothing dangles.
	 *
	 * THE COROLLARY IS THE THING TO KNOW, and it was measured rather than reasoned: on art the
	 * view is STILL DRAWING, a release frees nothing at all -- the next frame reloads every
	 * handle it just dropped. One -game run, release between two vacuus.TextureStats beats:
	 * 13 textures / 1.50 MiB before, 13 / 1.50 MiB after. So this API is for art a screen has
	 * stopped showing, and an auto-eviction policy built on it (VaCuus-dqs.3) MUST key on what
	 * was recently DRAWN rather than on what is merely cached, or it will burn decodes in a
	 * loop and free nothing.
	 *
	 * DEFAULT-EMPTY, the same "a host with no X owes nothing" shape DrainAsyncArrivals above
	 * uses: the ~two dozen test probe hosts have no render manager to ask.
	 */
	virtual bool ReleaseTextures(const FString& Source) { return false; }

	/**
	 * This view's RmlUi context, or null before Initialize() / after Shutdown().
	 *
	 * WHY THE HOST HANDS ITS CONTEXT OUT: input dispatch lives on the UI thread, in
	 * VaCuus, next to the FKey/modifier/button/wheel translation it needs
	 * (VaCuusInputMap) -- so the whole RmlUi input vocabulary stays in one module and
	 * SVaCuusWidget never includes an RmlUi header. The alternative, a
	 * ProcessInput(...) method here, would push that translation into whichever
	 * module implements the host. It is the mirror image of the snapshot, which the
	 * host already builds by handing its context to VaCuus code
	 * (BuildVaCuusInteractiveSnapshot).
	 *
	 * UI THREAD ONLY, and the pointer must not be stored: it dies with Shutdown().
	 */
	virtual Rml::Context* GetContext() const = 0;

	/**
	 * Records one frame (update layout, record draw commands) and publishes it.
	 * Only called while HasView() is true.
	 */
	virtual void RecordAndPublishFrame() = 0;
};

/**
 * "This decorator really does override IVaCuusDocumentHost::Method itself", checked by the
 * COMPILER rather than by review. Namespace scope, after the decorator's definition.
 *
 * WHY IT IS NEEDED AT ALL: every method above except DrainAsyncArrivals() is pure, so a
 * decorator that forgets one does not compile. DrainAsyncArrivals() has a default body (the
 * ~two dozen probe hosts in the test suites have no off-thread work and must not be forced to
 * write one), and a default body is exactly what a decorator can silently inherit -- which for
 * FVaCuusLobbyDemoHost would leave both lobby views draining nothing, i.e. bead akj.6.27
 * reinstated for the one production host that is wrapped.
 *
 * HOW IT WORKS, because it is not obvious: the type of `&C::m` is pointer-to-member-of-the-
 * class-that-DECLARES-m, not of the class it was named through ([expr.unary.op]/4). So
 * `decltype(&FVaCuusLobbyDemoHost::DrainAsyncArrivals)` is `void (FVaCuusLobbyDemoHost::*)()`
 * when the decorator declares its own override and `void (IVaCuusDocumentHost::*)()` when it
 * merely inherits the default -- and comparing those two types is the question. Overloading a
 * host method would break the `&C::m` form; none of them are overloaded, and this assert is
 * where that would be noticed.
 */
#define VACUUS_ASSERT_HOST_FORWARDS(DecoratorClass, Method)                                        \
	static_assert(                                                                                 \
		!std::is_same_v<decltype(&DecoratorClass::Method), decltype(&IVaCuusDocumentHost::Method)>, \
		#DecoratorClass " must declare and forward IVaCuusDocumentHost::" #Method                   \
						"(): inheriting the default body silences it for the host it wraps")
