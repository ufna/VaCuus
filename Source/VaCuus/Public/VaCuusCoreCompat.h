// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "Misc/EngineVersionComparison.h"
#include "RHIDefinitions.h"

/**
 * THE ENGINE-VERSION SEAM FOR EVERYTHING THAT IS NOT THE RENDER PATH — written by
 * Experiment SHIM-1's first real execution (5.6.1, 2026-08-10;
 * docs/passport/2026-08-vacuus-shim1.md).
 *
 * WHAT SHIM-1 ACTUALLY FOUND, and why this file exists next to
 * VaCuusRender/Private/VaCuusEngineCompat.h rather than inside it: the four hotspots
 * that header ranked as likeliest to drift are IDENTICAL in 5.6.1 and 5.8.1 — the
 * FDrawPassInputs field set, the FInputPreprocessorRegistrationKey overload with its
 * PreGame bucket, the FSceneInterface* SetParameters overload, and MakeCustom's
 * signature. Every real break was somewhere else, and half of them were outside
 * VaCuusRender entirely, so the "a version port is an edit to THIS file" promise could
 * not hold as written. It holds as a PAIR: render-path drift there, everything else
 * here. Neither file may grow a call site's worth of logic; they hold the seam only.
 *
 * This header lives in VaCuus/Public and not in the render module because VaCuusRender
 * depends on VaCuus and not the other way round, and because the one drift that spans
 * both modules — the material queries below — has a call site in each.
 *
 * WHY SO FEW #if GUARDS FOR SO MANY BREAKS: most of SHIM-1's breaks had a spelling that
 * is correct on BOTH engines, and a form that compiles everywhere always beats a version
 * test that has to be re-judged per release. Those fixes are at their call sites, each
 * with the citation that justifies it:
 *   - IPooledRenderTarget      -> include the umbrella RendererInterface.h, which 5.8
 *                                 still populates from the split-out header
 *                                 (VaCuusWorldSink.h)
 *   - FCookDependencyContext   -> __has_include on the header 5.8 split it into, since
 *                                 5.6 defines it in CookDependency.h itself
 *                                 (VaCuusBundle.cpp)
 *   - UE_FORCEINLINE_HINT      -> #ifndef fallback to FORCEINLINE, its own 5.8 default
 *                                 (VaCuusModelSampler.cpp)
 *   - FProperty constructors   -> overload-rank detection: 5.6 REQUIRES the EObjectFlags
 *                                 parameter and 5.8 DEPRECATED it in the same release it
 *                                 added the two-argument form, so no single spelling is
 *                                 warning-free (VaCuusModelLayoutTest.cpp)
 *   - FStringTable::SetSourceString -> overload-rank detection, because its arity
 *                                 depends on WITH_EDITORONLY_DATA on 5.8 and on nothing
 *                                 at all on 5.6 (VaCuusTranslationFoundationTest.cpp)
 *   - UStringTable auto-registration -> ask the registry instead of assuming: 5.6
 *                                 auto-registers every non-CDO table and 5.8 only assets,
 *                                 so a hand registration double-registers on 5.6 and the
 *                                 registry check()s (VaCuusTranslationFoundationTest.cpp)
 * Only the two below could not be written that way.
 */

namespace VaCuusCompat
{
// ---------------------------------------------------------------------------
// UMaterialInterface's resource and relevance queries changed what they are KEYED BY.
//
// 5.6: `GetMaterialResource(ERHIFeatureLevel::Type, EMaterialQualityLevel::Type =
// Num)` (5.6 MaterialInterface.h:603/609) and `GetRelevance_Concurrent(
// ERHIFeatureLevel::Type)` (5.6 MaterialInterface.h:761) — a feature level, and nothing
// else exists.
//
// 5.8: both take an EShaderPlatform (MaterialInterface.h:841/849, :1011), and the
// feature-level forms are still there but are `UE_DEPRECATED(5.7, "Please use
// GetMaterialResource with EShaderPlatform argument and not ERHIFeatureLevel::Type")`
// AND `final` (:839-840, :847-848, :1009-1010). Calling the old spelling on 5.8 would
// therefore compile a deprecation warning into every build that includes it — which is
// exactly the "keep the old form, it still works" trap.
//
// So this one genuinely needs the version test, and 5.7 is the boundary the 5.8 header
// itself names in that deprecation message. A wrong boundary cannot pass silently: it
// is a compile error at the two call sites on whichever release disagrees, not a
// behaviour change.
// ---------------------------------------------------------------------------

#if UE_VERSION_OLDER_THAN(5, 7, 0)
using FMaterialQueryTarget = ERHIFeatureLevel::Type;
#else
using FMaterialQueryTarget = EShaderPlatform;
#endif

/** What the material queries are keyed by for the RUNNING RHI (the game-side "current" material). */
inline FMaterialQueryTarget MaterialQueryTarget()
{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	return GMaxRHIFeatureLevel;
#else
	return GMaxRHIShaderPlatform;
#endif
}

/**
 * The same, for a SPECIFIC shader platform — a scene's, at proxy-construction time.
 * On the older engine the platform is narrowed to its feature level with the engine's
 * own mapping (GetMaxSupportedFeatureLevel, 5.6 DataDrivenShaderPlatformInfo.h:1010),
 * which is the conversion 5.6's own callers of the feature-level overload perform.
 */
inline FMaterialQueryTarget MaterialQueryTarget(EShaderPlatform InShaderPlatform)
{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	return GetMaxSupportedFeatureLevel(InShaderPlatform);
#else
	return InShaderPlatform;
#endif
}
}	 // namespace VaCuusCompat

// ---------------------------------------------------------------------------
// FTickableGameObject's STARTING tick type.
//
// 5.8: `FTickableGameObject(ETickableTickType StartingTickType = NewObject)`
// (Tickable.h:158). Constructing with Never does not register the object AT ALL
// (Tickable.cpp:135-144: the whole body is skipped), which is what makes it safe to
// construct a UObject on a worker thread — the non-Never path ensures IsInGameThread().
//
// 5.6: only `FTickableGameObject()` (5.6 Tickable.h:146), which unconditionally queues
// the object as ETickableTickType::NewObject (5.6 Tickable.cpp:130-136). The queue
// itself takes NewTickableObjectsCritical (5.6 Tickable.cpp:9-14), so the ADD is
// thread-safe; what the older engine cannot express is "do not register yet at all". The tick behaviour is
// nonetheless identical, because UVaCuusSubsystem::GetTickableTickType() already answers
// Never until bInitialized — 5.6 calls that virtual at the start of the next tick, and
// gets the same Never the 5.8 constructor argument states up front.
//
// Boundary: verified absent on 5.6.1, verified present on 5.8.1. 5.7 is where the
// project's other version test sits and is untested here; a wrong guess is a compile
// error at the one call site, not a silent behaviour change.
// ---------------------------------------------------------------------------
#if UE_VERSION_OLDER_THAN(5, 7, 0)
#define VACUUS_TICKABLE_STARTING_TYPE_ARG
#else
#define VACUUS_TICKABLE_STARTING_TYPE_ARG : FTickableGameObject(ETickableTickType::Never)
#endif
