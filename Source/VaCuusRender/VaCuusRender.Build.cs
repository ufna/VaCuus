// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;

// ============================================================================================
// THE SUPPORTED C++ SURFACE OF THIS PLUGIN, AND WHY IT IS THIS SHAPE (bead VaCuus-dgl).
//
// Public/ used to be whatever happened to land there. It is now a decision, and this is the
// decision record, because Build.cs is the file that actually DEFINES the boundary: UBT puts
// only a dependency's Public/ (plus its PUBLIC dependencies' Public/) on a consumer's include
// path, so "which header is where" and "which dep is public" are the same question.
//
// THE TWO HOSTS ARE THE SURFACE:
//   UVaCuusWidget          (VaCuusUMGWidget.h)     -- a view in a UMG tree, or in any Slate
//                                                     tree via UWidget::TakeWidget()
//   UVaCuusWorldComponent  (VaCuusWorldComponent.h) -- a view on a quad in the world
// plus UVaCuusView / UVaCuusSubsystem / UVaCuusStyleSet and friends, which the VaCuus module
// already exports. Between them a buyer can do all four things the docs promise: author a
// document, show it, feed it data, take input. Everything else in this module -- the recorder,
// the replayer's callers, the Slate element, the frame sinks, the RmlUi document host -- is
// the render backend and stays in Private/.
//
// ...AND SINCE 2026-08-07 THERE IS A THIRD DOOR, deliberately narrow (bead VaCuus-akj.25):
//   SVaCuusWidget          (SVaCuusWidget.h)   -- SUBCLASSABLE, for a UI that stacks views
//   VaCuusSlateView::MakeElement / MakeDocumentHost (VaCuusSlateView.h) -- opaque factories
//
// WHY THE RULE ABOVE DID NOT SURVIVE CONTACT. This block used to say, under the heading "WHY
// NO CONSTRUCTIBLE IVaCuusDocumentHost IS EXPORTED", that the two hosts were enough for every
// buyer verb. That was true of the FOUR VERBS and false of a real UI: stacked SVaCuusWidget
// siblings cannot compose, because Slate delivers a pointer event to the topmost hit-testable
// widget and an Unhandled reply from it goes to the GAME, never to a covered sibling. So a
// persistent chrome layer over a swapped content layer -- the single most ordinary shooter-
// lobby shape, and what this plugin's own lobby demo is -- needs one widget that owns input
// and routes between views itself. UVaCuusWidget cannot express that, and no amount of
// documentation makes it able to. The demo proved it by living INSIDE this module and
// reaching four private headers, which is exactly how a gap like this stays invisible.
//
// WHAT THE OLD ARGUMENT GOT RIGHT, AND WHY THE NEW DOOR DOES NOT REOPEN IT. The objection was
// never "hosts are secret"; it was that exporting FVaCuusRmlDocumentHost drags in
// IVaCuusFrameSink, then a CONCRETE sink, and with it ICustomSlateElement, the glass
// distiller, the pooled-RT destination-slot discipline and FVaCuusCommandBuffer's replay
// contract -- the whole render backend as a supported ABI. Every one of those is STILL in
// Private/. The factories hand back TSharedRef<FVaCuusSlateElement> to an INCOMPLETE type and
// TUniquePtr<IVaCuusDocumentHost> to the public interface: a caller can create the pair, pass
// it, and drop it, and can do nothing else with either. TSharedPtr's deleter is captured
// where the type was complete (VaCuusSlateView.cpp), which is what makes the handle safe to
// destroy across the boundary. So the promise is "these two objects exist and pair up", not
// "here is the backend".
//
// SVaCuusWidget IS the one real ABI widening, and it is the one that cannot be avoided:
// subclassing needs the type, the vtable and the input virtuals. Its header was already
// self-contained (engine includes only, FVaCuusSlateElement forward-declared), so the cost is
// the class itself and nothing behind it.
//
// So CreateView keeps a narrower, honest job: it is the EXTENSION SEAM for a caller who wants
// a view that this plugin does not host -- headless, offscreen, a test probe. That seam is
// genuinely reachable already, because IVaCuusDocumentHost is a pure interface in
// VaCuus/Public with no exported members to link against: implement it and call CreateView.
// The plugin does it from all four modules already, through the two shared test fixtures the
// probes now derive from -- VaCuus/Internal/VaCuusTestDocumentHost.h:44 (real context) and
// VaCuus/Internal/VaCuusTestNullDocumentHost.h:67 (no context at all, which is what VaCuusEditor
// uses, since it does not link VaCuusRml) -- plus their subclasses, e.g.
// VaCuusMultiViewTest.cpp:54 and VaCuusJs/Private/Tests/VaCuusJsDocumentTestHost.h:32. So the
// seam is load-bearing, not theoretical. docs/buyer/setup.md says the same thing in buyer words.
//
// AND THOSE FIXTURES ARE NOT PART OF THIS SURFACE, which is the point of Internal/ rather than
// Public/: UBT hands an Internal/ directory only to modules in the same Rules.Context.Scope
// (UEBuildModule.cs:736-740), i.e. to this plugin's own modules. A buyer cannot include them.
//
// LEFT ALONE, AND SAID SO RATHER THAN QUIETLY KEPT: VaCuusCommandBuffer.h and
// VaCuusReplayRenderer.h are in Public/ by history, not by decision -- nothing outside this
// module includes either (checked: every includer is under VaCuusRender/Private, plus
// VaCuusReplayRenderer.h itself). They belong in Private/ by the rule above, and
// FVaCuusReplayRenderer's export is half a promise anyway: it is exported, but every method
// takes an FVaCuusCommandBuffer whose destructor is NOT, so an outside caller cannot own one.
// Moving them is a pure-churn change with no buyer-facing effect, so it is a follow-up, not
// part of the VaCuus-dgl fix. Do not read their location as a promise.
//
// STILL UNREACHABLE ON PURPOSE, so nobody re-derives it: a host that publishes into a buyer's
// own UTextureRenderTarget2D. FVaCuusWorldSink can do it, but its destination slot must be
// re-pointed from the game thread after every RT (re)init (see its class comment), which is
// machinery, not an export -- a new feature with a new render path, not this fix.
//
// THE OBSERVABLE. None of this can be seen from inside the repository, so it is checked from
// outside the .so: `bash Tools/api_export_check.sh <BuildPlugin package with Binaries/>`. That
// script is the list above, executable, and it fails on the pre-fix package.
// ============================================================================================
public class VaCuusRender : ModuleRules
{
	public VaCuusRender(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",

			// Public: VaCuusReplayRenderer.h exposes FTextureRHIRef/FBufferRHIRef.
			"RHI",

			// PUBLIC, not private, since VaCuus-dgl moved VaCuusUMGWidget.h into Public/:
			// the header includes Components/Widget.h and the class derives from UWidget,
			// and a PRIVATE dependency is not propagated to consumers. KEEP IT PUBLIC --
			// what it propagates is the INCLUDE PATH, and that is real.
			//
			// WHAT IT DOES NOT DO, corrected 2026-08-05 (bead 724): this line does NOT
			// spare a buyer the link error. An earlier revision of this comment claimed,
			// under the heading "MEASURED, NOT ASSUMED", that promoting UMG here is what
			// prevents
			//     ld.lld: error: undefined symbol: UWidget::TakeWidget()
			// in a consumer module. It does not. UBT propagates a public dependency's
			// include paths transitively, but it does not add that module to a CONSUMER
			// module's link line: the recursive gather in SetupPublicLinkEnvironment is
			// gated on bIsModuleBinaryAStaticLibrary -- "Static libraries do not contain
			// the symbols for their dependencies, so we need to recursively gather them"
			// (UnrealBuildTool/Configuration/UEBuildModule.cs:950-958) -- and in a modular
			// editor build every module is its own .so, never a static library, so the
			// branch is never taken.
			//
			// Confirmed on the artifact, not derived: a real consumer project's
			// libUnrealEditor-<Game>.so.link.rsp carries -lUnrealEditor-UMG only because
			// that consumer module names UMG itself, and Slate/SlateCore are ABSENT from
			// that line entirely even though Engine re-exports both publicly
			// (Engine.Build.cs:89-90) -- which also falsifies this comment's old
			// parenthetical that Slate "needs no such promotion because Engine re-exports
			// it". Slate is equally not linked; it happens not to matter only because no
			// Slate symbol is referenced directly from a buyer's translation unit.
			//
			// THE ACTUAL FIX LIVES IN THE BUYER'S Build.cs, and docs/buyer/setup.md now
			// says so: a module that calls TakeWidget() must list "UMG" itself. Do not
			// "fix" this line by demoting it -- that would break the include path too, and
			// leave the buyer with a missing header on top of the missing symbol.
			"UMG",

			// PUBLIC because both exported classes name VaCuus types in their public
			// signatures -- UVaCuusWidget::GetView() and UVaCuusWorldComponent::GetView()
			// both hand back a UVaCuusView*, and UVaCuusWorldComponent::GetView() is inline,
			// so the buyer's compiler reads it. A type named in an exported signature has to
			// be reachable, or the export is a decoration: the handle would arrive as an
			// incomplete type and nothing could be called on it. (setup.md tells buyers to
			// list both modules themselves too; this line is what makes it work for one who
			// lists only VaCuusRender.)
			"VaCuus",

			// PROMOTED FROM PRIVATE by bead VaCuus-akj.25, which moved SVaCuusWidget.h into
			// Public/: the header derives from SLeafWidget (SlateCore) and names FKey
			// (InputCore) in AddPassThroughKey/GetPassThroughKeys. A private dependency's
			// include paths are not propagated, so leaving these private would hand a
			// consumer a public header it cannot compile.
			//
			// SAME CAVEAT AS "UMG" ABOVE, and for the same measured reason: this propagates
			// the INCLUDE PATH and not the link line. A module that subclasses
			// SVaCuusWidget must still list SlateCore itself to resolve SWidget's symbols.
			"SlateCore",
			"InputCore"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			// FPlatformApplicationMisc::RequiresVirtualKeyboard, the runtime signal
			// SVaCuusWidget::TickVirtualKeyboard arbitrates the IME and the on-screen
			// keyboard with (the same one SlateEditableTextLayout.cpp:879 uses). Engine
			// re-exports ApplicationCore only when bCompileAgainstApplicationCore is set,
			// and its include path arrives here via VaCuus in any case -- but the LINK
			// dependency does not, which this module found out as
			// `undefined symbol: FGenericPlatformApplicationMisc::RequiresVirtualKeyboard()`.
			// Same rule VaCuus.Build.cs states for InputCore: the dependency is ours, so it
			// is declared rather than inherited.
			"ApplicationCore",

			// PRIVATE, not public: no header under Public/ includes RmlUi, and none has
			// since M1's wrap-up moved VaCuusRecordingRenderInterface.h -- the one that
			// derives from Rml::RenderInterface -- into Private/. The record/replay pair
			// that stayed public (VaCuusCommandBuffer.h, VaCuusReplayRenderer.h) mirrors
			// RmlUi's types instead of including them, which is the whole point of
			// FVaCuusVertex and the uint64 handle aliases; the two hosts VaCuus-dgl added
			// to Public/ speak only UE types. Re-check this line if that ever changes.
			"VaCuusRml",

			// The EKeys::* FKey statics (mouse buttons in the widget's input path and in
			// VaCuus.Input.SlateRouting) are exported by InputCore, not by Engine's
			// re-export -- referencing them needs the link dependency.
			
			// Slate side of the render backend (widget, composite) lands in later
			// tasks; declared up front so the module shape is final.
			"RenderCore",
			"Renderer",
						"Slate",
			"Projects",

			// LoadTexture: synchronous dimension probe on the UI thread, async decode
			// on a worker (both through the module pointer cached at startup).
			"ImageWrapper"
		});
	}
}
