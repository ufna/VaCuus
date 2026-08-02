// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Framework/Application/SlateApplication.h"
#include "MaterialShader.h"
#include "RHICommandList.h"
#include "Rendering/DrawElements.h"
#include "Rendering/RenderingCommon.h"

/**
 * THE ENGINE-VERSION SEAM (M6 spec §2(f)/§3.3). The four engine call sites judged
 * likeliest to differ between UE 5.6/5.7 and the 5.8 this plugin is developed against
 * are routed through this one header, so a version port is an edit to THIS file — a
 * 5.6/5.7 compile break lands here, gets its `#if UE_VERSION_OLDER_THAN(...)` branch
 * here, and the call sites never change. The ranking is the M6 research's
 * (docs/research/m6-api-notes/buildplugin-fab.md §3, every 5.8 line opened); the
 * porting procedure is Experiment SHIM-1 (docs/passport/2026-08-vacuus-shim1.md).
 *
 * DELIBERATELY NO VERSION GUARDS TODAY: no 5.6/5.7 headers exist on this machine, so
 * any `#if` written now would assert an API shape nobody here has read — exactly the
 * comment-rot failure the project conventions forbid. Each hotspot instead documents
 * what 5.8 does (cited, opened) and what the porter must CHECK on the older engine.
 *
 * Everything else the plugin calls was surveyed by the same research and judged
 * stable by construction (ITextInputMethodContext/System are 4.x-era; IInputProcessor
 * virtuals, FScriptArrayHelper, the RDG utility family are unchanged across the
 * range; the batched scratch/commit shader-parameter pair — see hotspot 3's note;
 * quickjs/RmlUi are vendored) — those are NOT wrapped, because burying four live
 * hotspots under fifty inert pass-throughs would hide the seam's signal.
 */

namespace VaCuusCompat
{
// ---------------------------------------------------------------------------
// HOTSPOT 1 — ICustomSlateElement's RDG draw pass.
//
// 5.8: `virtual void Draw_RenderThread(FRDGBuilder&, const FDrawPassInputs&)`
// (RenderingCommon.h:958), with FDrawPassInputs the nested struct at :945-955.
// The RDG-builder SHAPE predates 5.6 — the same header still carries the
// UE_DEPRECATED(5.4) `DrawRenderThread(FRHICommandListImmediate&, const void*)`
// and the UE_DEPRECATED(5.5) FSlateCustomDrawParams overload (:963-979) — but
// FDrawPassInputs is a plain struct whose FIELD SET can drift per version with
// only a compile error to show it. The four fields the plugin reads are wrapped
// below; the override signature itself goes through the FVaCuusDrawPassInputs
// alias (bottom of file, outside the namespace).
//
// 5.6/5.7 CHECK: open RenderingCommon.h, find FDrawPassInputs, and repoint these
// four accessors at that version's names. The 5.5-deprecated params struct
// (:963-972) is the best guess at older spellings: ViewOffset for ElementsOffset,
// ViewRect for SceneViewRect, bIsHDR for bOutputIsHDRDisplay.
// ---------------------------------------------------------------------------

/** The Slate elements texture the element replays/composites into (5.8: `OutputTexture`, RenderingCommon.h:947). */
inline FRDGTexture* GetOutputTexture(const ICustomSlateElement::FDrawPassInputs& Inputs)
{
	return Inputs.OutputTexture;
}

/** Scene view rect inside the output texture (5.8: `SceneViewRect`, RenderingCommon.h:948) — glass sampling bounds. */
inline FIntRect GetSceneViewRect(const ICustomSlateElement::FDrawPassInputs& Inputs)
{
	return Inputs.SceneViewRect;
}

/** Window-to-elements-texture offset (5.8: `ElementsOffset`, RenderingCommon.h:950) — applied to every DestRect. */
inline FVector2f GetElementsOffset(const ICustomSlateElement::FDrawPassInputs& Inputs)
{
	return Inputs.ElementsOffset;
}

/**
 * Whether this pass renders to an HDR display target (5.8: `bOutputIsHDRDisplay`,
 * RenderingCommon.h:953). Note the field is RESET FALSE for the SDR elements pass
 * under HDR composite (SlateRHIRenderer.cpp:1068) — which is why glass gating also
 * has the game-thread r.HDR.EnableHDROutput mirror (SVaCuusWidget::OnPaint).
 */
inline bool IsHDRDisplayOutput(const ICustomSlateElement::FDrawPassInputs& Inputs)
{
	return Inputs.bOutputIsHDRDisplay;
}

// ---------------------------------------------------------------------------
// HOTSPOT 2 — input pre-processor registration with a priority bucket.
//
// 5.8: four RegisterInputPreProcessor overloads coexist (SlateApplication.h:
// 1544/1552/1560/1568); the plugin uses the newest — the
// FInputPreprocessorRegistrationKey form (:1568, struct at :222-233) — to land in
// the PreGame bucket: after the Engine/Editor buckets so PIE editor shortcuts stay
// ahead of us, before the Game bucket (EInputPreProcessorType at :189-208 —
// ascending order is earlier evaluation). The CommonUI shape
// (CommonUIActionRouterBase.cpp:353-358).
//
// 5.6/5.7 CHECK: whether the Key struct/overload exists. Fallback ladder,
// preserving the PreGame intent: the EInputPreProcessorType overload (:1560 on
// 5.8) if the enum exists; else the plain overload (:1544) — which registers in
// the (single) processor list and loses the before-Game guarantee, a behavior
// change to record in the port notes, not silently accept.
// ---------------------------------------------------------------------------

/** Registers Processor in the PreGame bucket, at the end of the bucket. Returns false if already registered. */
inline bool RegisterInputPreProcessor_PreGame(FSlateApplication& SlateApp, const TSharedPtr<class IInputProcessor>& Processor)
{
	FInputPreprocessorRegistrationKey Key;
	Key.Type = EInputPreProcessorType::PreGame;
	Key.Priority = INDEX_NONE;
	return SlateApp.RegisterInputPreProcessor(Processor, Key);
}

// ---------------------------------------------------------------------------
// HOTSPOT 3 — the scene-free FMaterialShader::SetParameters overload.
//
// 5.8: `const FSceneInterface*` overload at MaterialShader.h:88-92; null Scene is
// an explicitly handled input (ShaderBaseClasses.cpp:264: feature level falls back
// to GMaxRHIFeatureLevel, parameter collections to process defaults). ONE call
// site (VaCuusMaterialDraw.h). This overload is the research's genuinely
// unconfirmed item — "the FSceneInterface* overload's presence there is
// unconfirmed" — which is what earns it a wrapper.
//
// DELIBERATELY NOT WRAPPED alongside it (M6 review round 1): the batched
// scratch/commit plumbing — FRHICommandListBase::GetScratchShaderParameters
// (RHICommandList.h:996) / FRHICommandList::SetBatchedShaderParameters
// (RHICommandList.h:3683). It is used as seven get/commit pairs across three files
// of this module (the replay draw path, the glass path, the material path), the research
// rated it stable ("[inference] batched-parameters API predates 5.6", no
// deprecation churn near it in 5.8), and round 1 proved that wrapping it only
// where it was handy makes the seam's one-file promise false. It belongs to the
// surveyed-stable list in the header comment above; if SHIM-1 falsifies the
// inference, the port wraps ALL its sites in the same change.
//
// 5.6/5.7 CHECK: whether the FSceneInterface* overload exists there. If only the
// `const FSceneView&` form (:81-86 on 5.8) exists, the port must fabricate a
// minimal view — the Slate recipe the pass's view UB already follows
// (SlateRHIRenderingPolicy.cpp:706-756) — and that work lands HERE, not at the
// call site.
// ---------------------------------------------------------------------------

/** Material-specific (non-mesh) shader parameters with NO scene: the MaterialShader.h:88-92 overload, null Scene. */
inline void SetMaterialShaderParameters_NullScene(FMaterialShader& Shader, FRHIBatchedShaderParameters& BatchedParameters,
	const FMaterialRenderProxy* MaterialRenderProxy, const FMaterial& Material)
{
	Shader.SetParameters(BatchedParameters, MaterialRenderProxy, Material, static_cast<const FSceneInterface*>(nullptr));
}

// ---------------------------------------------------------------------------
// HOTSPOT 4 — handing a custom drawer to Slate.
//
// 5.8: FSlateDrawElement::MakeCustom is declared in DrawElementTypes.h:303, a
// header SPLIT OUT of DrawElements.h recently enough that the umbrella
// DrawElements.h still includes it (DrawElements.h:7). This file includes the
// umbrella, which resolves on 5.8 via the split header and on an engine that
// predates the split via the declaration living in DrawElements.h itself — the
// include is the shim.
//
// 5.6/5.7 CHECK: only that MakeCustom's signature still is
// (FSlateWindowElementList&, uint32, TSharedPtr<ICustomSlateElement>) — the
// wrapper turns any drift into a one-line fix here.
// ---------------------------------------------------------------------------

/** Adds the custom element to the window's draw list; Slate keeps the ref alive across the frame's render commands. */
inline void MakeCustomDrawElement(FSlateWindowElementList& ElementList, uint32 InLayer,
	const TSharedPtr<ICustomSlateElement, ESPMode::ThreadSafe>& CustomDrawer)
{
	FSlateDrawElement::MakeCustom(ElementList, InLayer, CustomDrawer);
}
} // namespace VaCuusCompat

/**
 * HOTSPOT 1's type name, outside the namespace because override declarations use it.
 * An alias, not a wrapper struct: the virtual's signature must match the engine's
 * exactly for `override` to compile, and the alias IS the engine type — its value is
 * that a 5.6/5.7 rename/move of the struct is fixed by editing this one line.
 */
using FVaCuusDrawPassInputs = ICustomSlateElement::FDrawPassInputs;
