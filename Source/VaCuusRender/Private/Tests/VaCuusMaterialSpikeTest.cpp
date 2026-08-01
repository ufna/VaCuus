// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusMaterialSpike.h"

#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The M5 material spike's automation coverage (spec §3.3 stage 2, GO deliverable): the
 * injected material path RUNS behind its cvar and is DORMANT without it, the MD_UI
 * refusal refuses, and the forced-republish gate term tracks the registry.
 *
 * WHAT THIS TEST HONESTLY OBSERVES, stated up front because the suite runs -nullrhi:
 * whether the injected draw ISSUES is observable (GetDrawCount / GetShaderMissCount —
 * the path counts both outcomes of the proxy walk), but pixels are not, and whether
 * TryGetShaders finds the FVaCuusMaterial* pair under a null RHI depends on the shader
 * map configuration of the run. So the ran/dormant assertions are exact (counter sums),
 * while draw-vs-miss is asserted only as "exactly one of the two". The PIXEL proof —
 * the material actually rendering, blending premultiplied over text, on Vulkan and in
 * the cooked monolithic game — is the spike's screenshot protocol
 * (docs/research/proofs/m5-t5-material-spike/), not this test.
 *
 * WHY DrawInjected_RenderThread IS DRIVEN DIRECTLY rather than through
 * FVaCuusReplayRenderer::Replay: ReplayCommands' very first act is three global-shader
 * TShaderMapRef binds, and under -nullrhi the global shader map does not carry them —
 * FGlobalShaderMapContent::GetShader's checkf(Shader.IsValid()) (GlobalShader.h:201)
 * fired and took the whole suite down when the first version of this test went through
 * Replay (observed 2026-08-01; no other test drives the replayer, which is why the suite
 * never hit it before). The call-site wiring — DrawInjected running inside the replay
 * pass — is exercised by every visual run instead.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusMaterialSpikeTest, "VaCuus.Render.Decorator.MaterialSpike",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusMaterialSpikeTest::RunTest(const FString& Parameters)
{
	IConsoleVariable* Master = IConsoleManager::Get().FindConsoleVariable(TEXT("vacuus.MaterialDecorators"));
	IConsoleVariable* Remedy = IConsoleManager::Get().FindConsoleVariable(TEXT("vacuus.MaterialForcedRepublish"));
	if (!TestNotNull(TEXT("vacuus.MaterialDecorators exists"), Master) ||
		!TestNotNull(TEXT("vacuus.MaterialForcedRepublish exists"), Remedy))
	{
		return false;
	}
	const int32 SavedMaster = Master->GetInt();
	const int32 SavedRemedy = Remedy->GetInt();
	ON_SCOPE_EXIT
	{
		VaCuusMaterialSpike::ClearAll();
		FlushRenderingCommands();
		Master->Set(SavedMaster, ECVF_SetByCode);
		Remedy->Set(SavedRemedy, ECVF_SetByCode);
	};

	// The committed spike assets (authored once in the editor — runtime-constructed
	// UMaterials cannot compile outside it; see the Task 5 report for the recipe).
	UMaterialInterface* UIMaterial =
		LoadObject<UMaterialInterface>(nullptr, TEXT("/VaCuus/Spike/M_VaCuusSpike_Translucent.M_VaCuusSpike_Translucent"));
	UMaterialInterface* SurfaceMaterial =
		LoadObject<UMaterialInterface>(nullptr, TEXT("/VaCuus/Spike/M_VaCuusSpike_WrongDomain.M_VaCuusSpike_WrongDomain"));
	if (!TestNotNull(TEXT("committed MD_UI spike material loads"), UIMaterial) ||
		!TestNotNull(TEXT("committed wrong-domain control loads"), SurfaceMaterial))
	{
		return false;
	}

	// The injected-draw entry point, driven on the render thread exactly as the replay
	// pass drives it (same arguments; see the header comment for why not through Replay).
	const auto RunReplay = []()
	{
		ENQUEUE_RENDER_COMMAND(VaCuusMatSpikeTestReplay)(
			[](FRHICommandListImmediate& RHICmdList)
			{
				VaCuusMaterialSpike::DrawInjected_RenderThread(RHICmdList, FIntPoint(256, 256), FMatrix44f::Identity);
			});
		FlushRenderingCommands();
	};

	// A legal outcome under -nullrhi is the named shader-miss Error (step 3 below) —
	// Occurrences=-1 is the documented "silently ignore if present" form
	// (AutomationTest.h, AddExpectedError doc), so a real-RHI run where the pair
	// resolves and the error never fires passes identically.
	AddExpectedError(TEXT("no FVaCuusMaterialVS/PS pair"), EAutomationExpectedErrorFlags::Contains, -1);

	// (1) The MD_UI refusal: a surface material never registers, so the gate term
	// cannot come up on its account.
	AddExpectedError(TEXT("is not a User Interface"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("surface-domain material refused"), VaCuusMaterialSpike::Register(SurfaceMaterial, FIntRect(0, 0, 8, 8)));
	TestFalse(TEXT("no forced republish after the refusal"), VaCuusMaterialSpike::WantsForcedRepublish());

	// (2) DORMANT while the cvar is off (the shipped default): a registered material
	// neither draws nor walks proxies, and the gate term stays down.
	Master->Set(0, ECVF_SetByCode);
	Remedy->Set(1, ECVF_SetByCode);
	TestTrue(TEXT("MD_UI material registers"), VaCuusMaterialSpike::Register(UIMaterial, FIntRect(16, 16, 128, 128)));
	FlushRenderingCommands(); // the registry mirror rides a render command
	TestFalse(TEXT("cvar off => no forced republish"), VaCuusMaterialSpike::WantsForcedRepublish());
	const int64 DormantDraws = VaCuusMaterialSpike::GetDrawCount();
	const int64 DormantMisses = VaCuusMaterialSpike::GetShaderMissCount();
	RunReplay();
	TestEqual(TEXT("cvar off => no injected draw"), VaCuusMaterialSpike::GetDrawCount(), DormantDraws);
	TestEqual(TEXT("cvar off => no proxy walk"), VaCuusMaterialSpike::GetShaderMissCount(), DormantMisses);

	// (3) LIVE with the cvar on: the gate term comes up, and one replay issues exactly
	// one injected attempt for the one registered material — a draw when the MD_UI
	// permutation resolves in this run's shader maps, a counted miss when it does not.
	Master->Set(1, ECVF_SetByCode);
	TestTrue(TEXT("cvar on + material live => forced republish"), VaCuusMaterialSpike::WantsForcedRepublish());
	Remedy->Set(0, ECVF_SetByCode);
	TestFalse(TEXT("remedy off => no forced republish (the freeze becomes observable)"), VaCuusMaterialSpike::WantsForcedRepublish());
	Remedy->Set(1, ECVF_SetByCode);

	RunReplay();
	const int64 NewDraws = VaCuusMaterialSpike::GetDrawCount() - DormantDraws;
	const int64 NewMisses = VaCuusMaterialSpike::GetShaderMissCount() - DormantMisses;
	TestEqual(TEXT("cvar on => exactly one injected attempt per replay"), NewDraws + NewMisses, int64(1));
	if (NewMisses > 0)
	{
		AddInfo(TEXT("attempt resolved as a shader MISS in this RHI/shader-map configuration (legal under -nullrhi); "
					 "the draw outcome is proven by the visual protocol"));
	}

	// (4) Cleared => dormant again, gate term down.
	VaCuusMaterialSpike::ClearAll();
	FlushRenderingCommands();
	TestFalse(TEXT("cleared => no forced republish"), VaCuusMaterialSpike::WantsForcedRepublish());
	const int64 ClearedDraws = VaCuusMaterialSpike::GetDrawCount();
	const int64 ClearedMisses = VaCuusMaterialSpike::GetShaderMissCount();
	RunReplay();
	TestEqual(TEXT("cleared => no injected draw"), VaCuusMaterialSpike::GetDrawCount(), ClearedDraws);
	TestEqual(TEXT("cleared => no proxy walk"), VaCuusMaterialSpike::GetShaderMissCount(), ClearedMisses);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
