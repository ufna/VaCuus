// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Tally of the two Rml::RenderInterface virtuals VaCuus answers with a REFUSAL rather than a
 * recording — SaveLayerAsTexture (reached only by `box-shadow`, GeometryBoxShadow.cpp:235) and
 * SaveLayerAsMaskImage (reached only by `mask-image`, ElementEffects.cpp:306). Both are optional
 * virtuals whose base returns zero (RenderInterface.cpp:37-45); leaving them there is what made
 * both properties fail silently through M5 (beads VaCuus-u0q, VaCuus-iuv).
 *
 * TWO NUMBERS PER PROPERTY, and the second is the whole point: Calls counts every refusal,
 * Warnings counts the log lines those refusals actually emitted. The latch is the invariant —
 * "a 200-element document logs once" — and an invariant with no observable cannot be tested.
 * Calls > 1 together with Warnings == 1 is that observable, and nothing else in the process can
 * see it: the automation framework does not capture Warnings, so reading the log from a test
 * would prove far less than this pair does.
 *
 * ITS OWN HEADER, small as it is, because both readers need it and neither wants the other's
 * includes: the recorder that writes it pulls in <RmlUi/Core/RenderInterface.h>, and
 * FVaCuusRmlDocumentHost — which re-exports it for tests — keeps RmlUi headers out of its own
 * header on purpose (see that class comment).
 *
 * A VALUE SNAPSHOT, not the storage. The counters live as atomics on the recorder: written on
 * the frame-owning thread, read from the game thread through the host. That is the same
 * cross-thread shape that made GetNumDecodeArrivals atomic, and unlike the frame counters, which
 * have an atomic mirror in FVaCuusViewStatus and so never need to be.
 */
struct FVaCuusUnsupportedTally
{
	/** SaveLayerAsTexture() calls refused — one per distinct `box-shadow` style, not per frame. */
	uint32 SaveLayerAsTextureCalls = 0;

	/** Log lines those refusals emitted. Latched: 1 once any refusal has happened. */
	uint32 SaveLayerAsTextureWarnings = 0;

	/** SaveLayerAsMaskImage() calls refused — one per masked element per frame. */
	uint32 SaveLayerAsMaskImageCalls = 0;

	/** Log lines those refusals emitted. Latched: 1 once any refusal has happened. */
	uint32 SaveLayerAsMaskImageWarnings = 0;
};
