// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// PRIVATE HEADER, which is what makes the RmlUi include legal here — the rule
// VaCuusDataVariable.h states in full: VaCuus depends on VaCuusRml privately, so RmlUi types
// may appear in this module's Private/ tree and nowhere else.
#include <RmlUi/Core/DataVariable.h>

namespace Rml
{
class Context;
class DataModelConstructor;
class DataModelHandle;
}	 // namespace Rml

/**
 * The LIVE half of localization (spec 2026-08-09 §1): text written `{{ t.key }}` inside a data
 * model re-translates in place when the game pushes a new table — no document reload, so no
 * lost JS state on the very screen where a player changes language.
 *
 * WHY IT IS A VARIABLE IN EVERY MODEL AND NOT A MODEL OF ITS OWN. The obvious design — one
 * `i18n` data model per context — cannot work. Element::SetParent (Element.cpp:2203-2218)
 * gives an element its parent's data model unless it declares `data-model=`, and that
 * declaration switches the WHOLE SUBTREE to the named model. One model per subtree, so inside
 * `<body data-model="hud">` an expression `{{ i18n.x }}` resolves against `hud` and finds
 * nothing. Translation therefore has to be an extra TOP-LEVEL VARIABLE in every model VaCuus
 * creates, which is what Bind() does.
 *
 * WHY IT UPDATES AT ALL, in one line of RmlUi: DataExpression::GetVariableNameList returns
 * `address[0].name` — the top-level name only (DataExpression.cpp:1145-1154) — and
 * DataModel::Update drives views off that dirty-name set (DataModel.cpp:373-378). So a single
 * DirtyVariable("t") re-evaluates every `{{ t.* }}` in the model, and this rides the exact
 * path bound models already use (FVaCuusBoundModel::ApplyPendingUpdate).
 *
 * KEYS ARE RESTRICTED, AND THAT IS WHY LIVE IS OPT-IN PER STRING. IsVariableCharacter
 * (DataExpression.cpp:315-330) allows a leading `a-zA-Z` then `a-zA-Z0-9`, `_` and `.` — no
 * hyphens, no spaces, no colons, no leading digit. A key that does not fit simply stays
 * parse-time and translates through FVaCuusSystemInterface::TranslateString as before. Made a
 * global mode instead, that character table would become a naming constraint on every key in
 * the project.
 *
 * DOTTED KEYS WORK: ParseAddress splits on '.' (DataModel.cpp:9-42), so `{{ t.menu.title }}`
 * arrives as three Child() calls. Each one accumulates the prefix and the leaf looks up the
 * joined key, because hierarchical keys are the dominant style in real projects.
 *
 * THREAD: UI thread throughout, like every other RmlUi-facing surface here. The definition is
 * a function-local static owned by this file; RmlUi stores a raw VariableDefinition* inside
 * every DataVariable, so it must outlive every model — same lifetime argument as
 * FVaCuusDefinitionRegistry, and the same teardown point (FVaCuusUIThread::Exit).
 */
namespace VaCuusTranslationVariable
{
/**
 * The reserved top-level name, `t`. Compared BYTE-EXACT, not case-folded: RmlUi resolves data
 * addresses byte-for-byte, so a struct field spelled `T` is a different variable and is not in
 * conflict with this one.
 */
extern const TCHAR* const ReservedName;

/**
 * Binds `t` into a model under construction. UI thread.
 *
 * CALL IT BEFORE the game's own variables, so that a struct which also declares a top-level
 * `t` loses deterministically and gets told why by the caller, rather than winning a race and
 * silently disabling live translation for that model.
 *
 * @return false if RmlUi refused the bind (only possible when the name is already taken).
 */
bool Bind(Rml::DataModelConstructor& Constructor);

/** Dirties `t` so every `{{ t.* }}` in this model re-evaluates on the next Update. UI thread. */
void Dirty(Rml::DataModelHandle& Handle);

/**
 * Creates this view's STANDALONE translation model, named `vacuus`, holding `t` and nothing
 * else. UI thread, called from AddView — the model must exist before any document loads,
 * because `data-model=` is resolved once, at element attach (Element.cpp:2211).
 *
 * IT EXISTS FOR THE DOCUMENT WITH NO GAME MODEL. Data expressions do nothing at all outside a
 * `data-model` subtree, so without this a HUD that binds no struct could not use `{{ t.key }}`
 * — and a settings screen, the one place a language actually changes, is exactly the kind of
 * document that often binds nothing. Such a document writes `<body data-model="vacuus">`.
 *
 * THE HANDLES LIVE HERE RATHER THAN ON FVaCuusUIThread because that class is a PUBLIC header
 * and Rml::DataModelHandle is an RmlUi type; the module's rule is that RmlUi appears in
 * Private/ only (VaCuusDataVariable.h states it).
 *
 * @return false if the context already had a model of that name (RmlUi logs its own error).
 */
bool CreateStandaloneModel(uint32 ViewId, Rml::Context& Context);

/**
 * Forgets this view's standalone model. UI thread, called from RemoveView BEFORE the host's
 * Shutdown(): the handle is a raw pointer into a DataModel the context owns, so it must stop
 * being reachable before Rml::RemoveContext destroys it. Safe for a view that never had one.
 */
void DropStandaloneModel(uint32 ViewId);

/** Dirties `t` on every standalone model. UI thread; the drain's other half beside the bound models. */
void DirtyStandaloneModels();

/** How many standalone models are live. UI thread. */
int32 GetNumStandaloneModels();

/** How many `{{ t.* }}` values have been read. The live route's observable. UI thread. */
uint64 GetNumGets();

/** How many times a model has been dirtied through Dirty(). UI thread. */
uint64 GetNumDirties();

/**
 * How many distinct key paths are interned. The pool grows with the DISTINCT `{{ t.* }}` keys
 * a process has ever evaluated, never with frames, so this is also the leak observable.
 */
int32 GetNumInternedKeys();

/**
 * UI-thread teardown, called from FVaCuusUIThread::Exit() beside
 * FVaCuusDefinitionRegistry::ReleaseAll() and for the weaker version of the same reason: no
 * UObject is held here, but a DataVariable's void* points into this pool, so it may only be
 * dropped once every context is gone.
 *
 * @return how many key paths were released.
 */
int32 ReleaseAll();
}	 // namespace VaCuusTranslationVariable
