// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include <RmlUi/Core/SystemInterface.h>

/**
 * Routes RmlUi time queries, log output and cursor requests to UE
 * (FPlatformTime / LogVaCuus / EMouseCursor).
 */
class FVaCuusSystemInterface : public Rml::SystemInterface
{
public:
	FVaCuusSystemInterface();

	//~ Begin Rml::SystemInterface
	virtual double GetElapsedTime() override;
	virtual bool LogMessage(Rml::Log::Type Type, const Rml::String& Message) override;

	/**
	 * The RML-text half of localization (M5 Task 8, spec §2(l)): whole-string
	 * lookup through the same immutable snapshot `vacuus.translate` reads
	 * (FVaCuusTranslationRegistry). RmlUi calls this at TEXT INSTANCING — every
	 * parsed text chunk (Factory.cpp:336), data-bound text re-evaluation
	 * (DataViewDefault.cpp:369), textarea/title (XMLNodeHandlerTextArea.cpp:43,
	 * XMLNodeHandlerHead.cpp:123) — never again for text already in the tree, so a
	 * new table reaches loaded documents only through a reload (the header of
	 * UVaCuusSubsystem::SetTranslationTable carries the workflow).
	 *
	 * KEYS ARE THE WHOLE STRING, verbatim: RmlUi hands the entire text run here,
	 * so the table's keys are the authored strings ("HUD_TITLE" if that is what
	 * the RML says). A miss is identity, silent by design — this runs for every
	 * text chunk of every document, translation intended or not, so any per-miss
	 * log would be noise the moment one table exists; the named no-table refusal
	 * lives on the JS hook, whose caller asked for localization by name.
	 *
	 * Called from inside RmlUi on the UI thread by construction (the
	 * ActivateKeyboard argument above), which is the snapshot's owner thread.
	 */
	virtual int TranslateString(Rml::String& Translated, const Rml::String& Input) override;

	/**
	 * Cursor shape is PUSH-based, and this is the only place it can be caught.
	 * RmlUi calls this from inside Context::Update's hover-chain pass and ONLY when
	 * the name changed (Context.cpp:1315-1327); there is no "what cursor do you want"
	 * query to answer from the game thread. So the name is latched here and each
	 * host picks it up right after its own Update() -- see
	 * GetVaCuusLatchedMouseCursor() for why that attributes the change to the right
	 * view.
	 */
	virtual void SetMouseCursor(const Rml::String& CursorName) override;

	/**
	 * The caret, and the only place it can be caught -- see GetVaCuusLatchedCaret().
	 *
	 * RmlUi means this as "show the on-screen keyboard here"; for a desktop embedder it is
	 * the IME candidate-window anchor, and it is the ONLY per-caret geometry RmlUi ever
	 * hands out (`Rml::TextInputContext::GetBoundingBox` is the element's whole border box).
	 * CaretPosition is in absolute RmlUi context pixels; latched, never acted on here,
	 * because the host that owns this context has to attribute it first.
	 */
	virtual void ActivateKeyboard(Rml::Vector2f CaretPosition, float LineHeight) override;

	/** Focus left every text field: there is no caret to follow until the next Activate. */
	virtual void DeactivateKeyboard() override;
	//~ End Rml::SystemInterface

private:
	/** FPlatformTime::Seconds() at construction; GetElapsedTime is relative to this. */
	double StartTime = 0.0;
};
