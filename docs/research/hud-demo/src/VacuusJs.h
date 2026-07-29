// VacuusJs: minimal QuickJS-ng <-> RmlUi bindings for the VaCuus HUD demo.
// Demo-grade surface only -- NOT a general DOM.
#pragma once

namespace Rml {
class Context;
class ElementDocument;
} // namespace Rml

namespace VacuusJs {

// Create the JS runtime/context and install the `vacuus` global plus
// console/setTimeout/setInterval/requestAnimationFrame.
bool Initialize(Rml::Context* rml_context, Rml::ElementDocument* document, double auto_exit_seconds);

// Free all JS resources. Call AFTER the RmlUi document tree has been
// destroyed (doc->Close() + context->Update()) and BEFORE Rml::Shutdown().
void Shutdown();

// Evaluate a JS file in the global scope. Returns false on exception.
bool EvalFile(const char* path);

// Per-frame driver: fires requestAnimationFrame callbacks, due timers and
// pending JS jobs. Call once per frame BEFORE Context::Update().
void OnFrame(double now_seconds);

// Forward a key press to JS (calls the `vacuus.onKey` property if set).
void OnKey(const char* key_name);

// Feed the C++-measured stats that vacuus.stats() reports.
void SetStats(double update_ms, double render_ms, double fps);

// True once JS called vacuus.exit().
bool ExitRequested();

// Number of JS exceptions dumped so far (smoke-test bookkeeping).
int ErrorCount();

} // namespace VacuusJs
