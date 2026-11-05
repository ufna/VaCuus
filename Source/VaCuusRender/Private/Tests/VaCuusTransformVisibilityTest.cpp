// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusCommandBuffer.h"
#include "VaCuusEngine.h"
#include "VaCuusRecordingRenderInterface.h"

#include <RmlUi/Core.h>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The re-vendoring check for RmlUi Patch #7 (Source/ThirdParty/RmlUi/VENDORED_TAG.txt): the backport
 * of upstream mikke89/RmlUi 6df503c, "Fix transform not being applied after element turns visible,
 * see #982". The MECHANISM -- why an element that joins a transformed ancestor's stacking context
 * after that ancestor's matrix last changed is never told to compute its own -- lives in that entry
 * and is deliberately not repeated here: two copies of one file:line trail is the thing that rots
 * (VENDORED_TAG.txt, Patch #1). This header holds only what is specific to the rig.
 *
 * WHAT IS PORTED. Upstream's doctest `Element.TransformStateAfterVisibilityChange` (added to
 * Tests/Source/UnitTests/Element.cpp by that commit; this tree vendors no Tests/ at all) has two
 * SUBCASEs, `display: none -> block` and `visibility: hidden -> visible`. AfterVisibilityChange
 * carries both. They reach the patched loop through the same `visible` recompute
 * (Element.cpp:1855-1858), but `display` is registered as layout-affecting and `visibility` is not
 * (StyleSheetSpecification.cpp:306 vs :339), so the visibility flip is the purer probe: a future change
 * that dirtied transforms from the layout pass would keep the display case green and lose this one.
 * AfterLateInsert is not upstream's; it covers the second route the same missing dirty reaches in a
 * VaCuus document, the `data-for` clone whose content is created under a parent that has not rendered
 * yet (the SetParent bullet of the Patch #7 entry).
 *
 * WHY THE ASSERTIONS READ THE RECORDED SetTransform STREAM rather than the element's own state. The
 * null-ness of `Element::GetTransformState()` IS observable from here -- the accessor is public
 * (Include/RmlUi/Core/Element.h:576) and VaCuusInteractiveSnapshot.cpp:547 null-checks it across the
 * same module boundary -- so a TestNull/TestNotNull pair would prove the RmlUi fix in four lines. The
 * stream is read instead because what VaCuus ships is the command buffer: the recorder is the seam the
 * replayer consumes, so "the matrix reaches the buffer" is the assertion a re-vendor has to keep.
 * Comparing matrices directly, as upstream's doctest does, is the one thing genuinely unavailable:
 * it needs the complete TransformState type, which is private to Source/Core (TransformState.h:8, no
 * export). The stream gives that back. RmlUi emits a SetTransform command only when the active matrix
 * actually changes (RenderManager::SetTransform, RenderManager.cpp:144-154), and
 * ElementUtilities::ApplyTransform passes nullptr for an element without a state
 * (ElementUtilities.cpp:383-387), so a member stuck without one forces an explicit identity into the
 * recording, which the recorder stores as FVaCuusCommand's default Transform.
 *
 * WHY TARGETS ARE COMPARED AGAINST pane_reference's RECORDED MATRIX rather than a hand-built scale(2).
 * `Element::UpdateTransformState` folds `transform-origin` into the resolved matrix as
 * `Translate(origin) * transform * Translate(-origin)` (Element.cpp:2995), and the default origin is
 * 50% 50% by property registration (StyleSheetSpecification.cpp:394-395) -- not the centre-of-box
 * initialiser at Element.cpp:2980, which both branches of the if/else below it overwrite. For this
 * 100x100 `#window` that is `[2 0 0 0][0 2 0 0][0 0 1 0][-50 -50 0 1]` in FMatrix44f's row-vector
 * layout. Comparing against the always-visible reference pane in the same frame keeps that arithmetic
 * out of the assertion: with the patch every pane draws under #window's one matrix, without it a
 * target reads identity while the reference does not. Element.cpp lines here and in the Patch #7 entry
 * are for this tree WITH the hunk applied; upstream's pre-patch numbers are 7 lower from line 2380 on.
 */
namespace VaCuusTransformVisibilityTest
{
static const FIntPoint GViewSize(400, 400);

/** One recorded frame: Update (styles/layout; a display/visibility change or a DOM insert dirties the
 *  closest stacking-context container here, Element.cpp:2489-2493) then Render (issues the
 *  RenderInterface calls this test inspects). Same shape as VaCuusDecoratorTest.cpp's. */
static TUniquePtr<FVaCuusCommandBuffer> RecordContextFrame(FVaCuusRecordingRenderInterface& Recorder, Rml::Context* Context)
{
	Recorder.BeginFrame(GViewSize);
	Context->Update();
	Context->Render();
	return Recorder.EndFrameAndPublish();
}

/** The transform active at the moment Buffer.Commands[DrawIndex] was recorded: whichever SetTransform
 *  most recently preceded it in the stream, identity if none did (the dedupe at RenderManager.cpp:149
 *  means a matrix is only ever recorded on change). */
static FMatrix44f ActiveTransformBefore(const FVaCuusCommandBuffer& Buffer, int32 DrawIndex)
{
	FMatrix44f Current = FMatrix44f::Identity;
	for (int32 Index = 0; Index < DrawIndex; ++Index)
	{
		if (Buffer.Commands[Index].Type == EVaCuusCommandType::SetTransform)
		{
			Current = Buffer.Commands[Index].Transform;
		}
	}
	return Current;
}

/** Index of the DrawGeometry whose Translation matches ExpectedTranslation, or INDEX_NONE. A background
 *  draws at the element's absolute border offset (ElementBackgroundBorder.cpp:41-42), ROUNDED before it
 *  is recorded (Geometry.cpp:13) and never transformed -- the transform is a separate command -- so an
 *  integral layout offset identifies the element independently of the bug under test. Keep fixture
 *  offsets integral, or round the expectation. */
static int32 FindDrawAt(const FVaCuusCommandBuffer& Buffer, const FVector2f& ExpectedTranslation)
{
	for (int32 Index = 0; Index < Buffer.Commands.Num(); ++Index)
	{
		const FVaCuusCommand& Command = Buffer.Commands[Index];
		if (Command.Type == EVaCuusCommandType::DrawGeometry && Command.Translation.Equals(ExpectedTranslation, 0.01f))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

/**
 * Boots RmlUi around one recording context and tears it down in the order RmlUi's lifetime contract
 * requires: the render interface must outlive Rml::Shutdown (Include/RmlUi/Core/Core.h:78-80), because
 * Shutdown releases the font textures (Core.cpp:181) and then the render managers (Core.cpp:183)
 * THROUGH it, after RemoveContext has only erased the context (Core.cpp:305-308). A destructor body runs
 * before the members are destroyed, so Recorder, a member, is alive for the whole of Shutdown by shape
 * -- no ON_SCOPE_EXIT ordering to get wrong. (These fixtures have no text and no images, so a wrong
 * order would be latent here; it becomes a use-after-scope in the next test's boot the day someone adds
 * a <p>. VaCuusRecorderTest.cpp:721 declares its recorder before the engine for the same reason.)
 */
struct FRig
{
	FVaCuusRecordingRenderInterface Recorder;
	Rml::String ContextName;
	Rml::Context* Context = nullptr;
	Rml::ElementDocument* Document = nullptr;
	bool bBooted = false;

	/** ExtraStyle and Body are dropped into the shared fixture: #window is a local stacking context
	 *  whose scale(2) resolves on the first render, and every .pane is absolutely positioned against
	 *  it, so a pane's absolute offset is its own left/top whatever wraps it. Panes are 20x20 magenta,
	 *  one DrawGeometry each. */
	bool Boot(FAutomationTestBase& Test, const char* Name, const FString& ExtraStyle, const FString& Body)
	{
		if (!Test.TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
		{
			return false;
		}
		if (!Test.TestTrue(TEXT("Initialized"), FVaCuusEngine::Get().Initialize()))
		{
			return false;
		}
		bBooted = true;

		ContextName = Name;
		Context = Rml::CreateContext(ContextName, Rml::Vector2i(GViewSize.X, GViewSize.Y), &Recorder);
		if (!Test.TestNotNull(TEXT("Context"), Context))
		{
			return false;
		}

		const FString Source = FString(TEXT("<rml><head><style>"))
			+ TEXT("body{display:block;width:100%;height:100%;}")
			+ TEXT("#window{display:block;position:absolute;left:0;top:0;width:100px;height:100px;transform:scale(2);}")
			+ TEXT(".pane{display:block;position:absolute;width:20px;height:20px;background-color:#ff00ff;}")
			+ ExtraStyle + TEXT("</style></head><body>") + Body + TEXT("</body></rml>");
		Document = Context->LoadDocumentFromMemory(Rml::String(TCHAR_TO_UTF8(*Source)), "vacuus://transform_visibility.rml");
		if (!Test.TestNotNull(TEXT("Document"), Document))
		{
			return false;
		}
		Document->Show();
		return true;
	}

	~FRig()
	{
		if (Context)
		{
			Rml::RemoveContext(ContextName);
		}
		if (bBooted)
		{
			FVaCuusEngine::Get().Shutdown();
		}
	}
};

static const FVector2f GReferenceTranslation(10.f, 10.f);

/** pane_reference's matrix in Buffer, after asserting that it draws and not under identity: the
 *  positive control that the rig applies #window's transform at all, so a later identity is the bug
 *  and not a broken fixture. It is recorded afresh every frame because Context::Render ends in
 *  ResetState (Context.cpp:239), which returns the active matrix to identity (RenderManager.cpp:187-190,
 *  RenderManager.h:37) and so defeats the dedupe across frames. */
static bool ReferenceTransformIn(FAutomationTestBase& Test, const FVaCuusCommandBuffer& Buffer, const TCHAR* Frame, FMatrix44f& OutTransform)
{
	const int32 Draw = FindDrawAt(Buffer, GReferenceTranslation);
	if (!Test.TestTrue(FString::Printf(TEXT("%s: pane_reference draws"), Frame), Draw != INDEX_NONE))
	{
		return false;
	}
	OutTransform = ActiveTransformBefore(Buffer, Draw);
	return Test.TestFalse(FString::Printf(TEXT("%s: pane_reference draws under a non-identity transform (positive control)"), Frame),
		OutTransform.Equals(FMatrix44f::Identity, 1e-4f));
}

/** The assertion both tests share: the pane at Translation draws in Buffer, and under exactly the
 *  reference pane's matrix. That matrix is #window's own, put in force by #window's enter-stage
 *  ApplyTransform (Element.cpp:191-192) before any child renders; each member's ApplyTransform of the
 *  same inherited matrix is then a dedupe no-op, and a member with NO state instead forces the
 *  explicit identity described in the file header. Translation is right in both cases, only the
 *  matrix tells them apart. */
static void TestDrawsUnderReference(FAutomationTestBase& Test, const FVaCuusCommandBuffer& Buffer, const TCHAR* Pane,
	const FVector2f& Translation, const FMatrix44f& ReferenceTransform)
{
	const int32 Draw = FindDrawAt(Buffer, Translation);
	if (!Test.TestTrue(FString::Printf(TEXT("%s draws, at its unscaled layout offset"), Pane), Draw != INDEX_NONE))
	{
		return;
	}
	const FMatrix44f Transform = ActiveTransformBefore(Buffer, Draw);
	Test.TestTrue(FString::Printf(TEXT("%s draws under pane_reference's matrix (#window's resolved transform)"), Pane),
		Transform.Equals(ReferenceTransform, 1e-4f));
	Test.TestFalse(FString::Printf(TEXT("%s draws under a non-identity transform"), Pane), Transform.Equals(FMatrix44f::Identity, 1e-4f));
}
} // namespace VaCuusTransformVisibilityTest

/**
 * THE RESTORE-THE-BUG CASE, upstream's shape. Two panes become visible AFTER #window's scale(2) has
 * been resolved by an earlier frame, one through `display`, one through `visibility` -- the ordering
 * a `data-if` (inline `display: none`, DataViewDefault.cpp:264-266) produces inside a transformed
 * panel. Plain property flips keep the test independent of VaCuus's data-binding layer.
 *
 * Without Patch #7 both targets draw at their correct, unscaled layout offsets but under an explicit
 * identity, and the four matrix assertions fail; with it they draw under the reference pane's matrix.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusTransformAfterVisibilityChangeTest, "VaCuus.Render.Transform.AfterVisibilityChange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusTransformAfterVisibilityChangeTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusTransformVisibilityTest;

	FRig Rig;
	if (!Rig.Boot(*this, "vacuus_transform_visibility_test",
			TEXT("#pane_reference{left:10px;top:10px;}")
			TEXT("#pane_display{left:50px;top:10px;display:none;}")
			TEXT("#pane_visibility{left:10px;top:50px;visibility:hidden;}"),
			TEXT("<div id=\"window\">")
			TEXT("<div class=\"pane\" id=\"pane_reference\"/>")
			TEXT("<div class=\"pane\" id=\"pane_display\"/>")
			TEXT("<div class=\"pane\" id=\"pane_visibility\"/>")
			TEXT("</div>")))
	{
		return false;
	}

	const FVector2f DisplayTranslation(50.f, 10.f);
	const FVector2f VisibilityTranslation(10.f, 50.f);

	// Frame 1: #window's matrix resolves; the reference proves it is applied; neither hidden pane
	// draws. AddToStackingContext skips both alike (Element.cpp:2397-2398), so neither is a member of
	// #window's stacking context while its matrix is being propagated.
	const TUniquePtr<FVaCuusCommandBuffer> First = RecordContextFrame(Rig.Recorder, Rig.Context);
	if (!TestNotNull(TEXT("Frame 1 publishes"), First.Get()))
	{
		return false;
	}
	FMatrix44f ReferenceFrame1;
	ReferenceTransformIn(*this, *First, TEXT("frame 1"), ReferenceFrame1);
	TestTrue(TEXT("pane_display does not draw while display:none"), FindDrawAt(*First, DisplayTranslation) == INDEX_NONE);
	TestTrue(TEXT("pane_visibility does not draw while visibility:hidden"), FindDrawAt(*First, VisibilityTranslation) == INDEX_NONE);

	// The flips. Each changes the element's effective visibility, which dirties #window's stacking
	// context (Element.cpp:1859-1864); both panes join it on the next render, after the matrix they
	// need last changed.
	Rml::Element* Display = Rig.Document->GetElementById("pane_display");
	Rml::Element* Visibility = Rig.Document->GetElementById("pane_visibility");
	if (!TestNotNull(TEXT("pane_display element"), Display) || !TestNotNull(TEXT("pane_visibility element"), Visibility))
	{
		return false;
	}
	Display->SetProperty("display", "block");
	Visibility->SetProperty("visibility", "visible");

	const TUniquePtr<FVaCuusCommandBuffer> Second = RecordContextFrame(Rig.Recorder, Rig.Context);
	if (!TestNotNull(TEXT("Frame 2 (after the flips) publishes"), Second.Get()))
	{
		return false;
	}
	FMatrix44f ReferenceFrame2;
	if (!ReferenceTransformIn(*this, *Second, TEXT("frame 2"), ReferenceFrame2))
	{
		return false;
	}

	// THE ASSERTIONS.
	TestDrawsUnderReference(*this, *Second, TEXT("pane_display (display:none -> block)"), DisplayTranslation, ReferenceFrame2);
	TestDrawsUnderReference(*this, *Second, TEXT("pane_visibility (visibility:hidden -> visible)"), VisibilityTranslation, ReferenceFrame2);

	return true;
}

/**
 * THE SECOND ROUTE: content created under a parent that has not rendered yet, which is what
 * DataViewFor::Update does to every clone (InsertBefore at DataViewDefault.cpp:563, then SetInnerRML
 * at :567). Two inserts after #window's matrix has resolved:
 *
 * - pane_direct, a leaf appended straight under #window, is the CONTROL. #window holds a
 *   transform_state, so SetParent dirties the leaf (Element.cpp:2190-2192) and it renders correctly
 *   WITHOUT the hunk. It is asserted here so the distinction is enforced rather than remembered:
 *   simplify pane_nested to this shape and the test can no longer fail.
 * - pane_nested is THE CASE. Its wrapper is appended first (dirtied: its parent has a state) and
 *   filled while the wrapper's own state is still null, so the content is parented without a dirty.
 *   The wrapper's first-render propagation (Element.cpp:3037-3041) then walks the wrapper's OWN
 *   stacking_context, which is empty because a plain block is not a stacking-context container;
 *   #window's flat list is where the content actually lives, and pre-patch nothing dirties that
 *   list's members. Without Patch #7 pane_nested draws under identity; with it, #window's rebuild
 *   dirties every member of the rebuilt list and it draws under #window's matrix.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusTransformAfterLateInsertTest, "VaCuus.Render.Transform.AfterLateInsert",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusTransformAfterLateInsertTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusTransformVisibilityTest;

	FRig Rig;
	if (!Rig.Boot(*this, "vacuus_transform_late_insert_test",
			TEXT("#pane_reference{left:10px;top:10px;}")
			TEXT("#pane_direct{left:50px;top:10px;}")
			TEXT("#pane_nested{left:10px;top:50px;}")
			TEXT(".wrap{display:block;}"),
			TEXT("<div id=\"window\"><div class=\"pane\" id=\"pane_reference\"/></div>")))
	{
		return false;
	}

	const FVector2f DirectTranslation(50.f, 10.f);
	const FVector2f NestedTranslation(10.f, 50.f);

	// Frame 1: the matrix resolves with only the reference inside.
	const TUniquePtr<FVaCuusCommandBuffer> First = RecordContextFrame(Rig.Recorder, Rig.Context);
	if (!TestNotNull(TEXT("Frame 1 publishes"), First.Get()))
	{
		return false;
	}
	FMatrix44f ReferenceFrame1;
	ReferenceTransformIn(*this, *First, TEXT("frame 1"), ReferenceFrame1);
	TestTrue(TEXT("nothing draws at pane_direct's offset before the insert"), FindDrawAt(*First, DirectTranslation) == INDEX_NONE);
	TestTrue(TEXT("nothing draws at pane_nested's offset before the insert"), FindDrawAt(*First, NestedTranslation) == INDEX_NONE);

	Rml::Element* Window = Rig.Document->GetElementById("window");
	if (!TestNotNull(TEXT("window element"), Window))
	{
		return false;
	}

	// The control: leaf straight under the transformed container.
	{
		Rml::ElementPtr Leaf = Rig.Document->CreateElement("div");
		Leaf->SetClass("pane", true);
		Leaf->SetId("pane_direct");
		Window->AppendChild(std::move(Leaf));
	}

	// The case: the DataViewFor shape -- parent first, content second, no render in between.
	{
		Rml::ElementPtr WrapperPtr = Rig.Document->CreateElement("div");
		WrapperPtr->SetClass("wrap", true);
		Rml::Element* Wrapper = Window->AppendChild(std::move(WrapperPtr));
		Wrapper->SetInnerRML("<div class=\"pane\" id=\"pane_nested\"/>");
	}

	const TUniquePtr<FVaCuusCommandBuffer> Second = RecordContextFrame(Rig.Recorder, Rig.Context);
	if (!TestNotNull(TEXT("Frame 2 (after the inserts) publishes"), Second.Get()))
	{
		return false;
	}
	FMatrix44f ReferenceFrame2;
	if (!ReferenceTransformIn(*this, *Second, TEXT("frame 2"), ReferenceFrame2))
	{
		return false;
	}

	// The control holds with or without the hunk; THE ASSERTION is pane_nested.
	TestDrawsUnderReference(*this, *Second, TEXT("pane_direct (leaf appended under #window, the control)"), DirectTranslation, ReferenceFrame2);
	TestDrawsUnderReference(*this, *Second, TEXT("pane_nested (content filled into an unrendered wrapper)"), NestedTranslation, ReferenceFrame2);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
