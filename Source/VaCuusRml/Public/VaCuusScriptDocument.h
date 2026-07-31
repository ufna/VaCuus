// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

// For DLLEXPORT/DLLIMPORT, which the UBT-generated VACUUSRML_API expands to --
// the one UE header this module's public surface needs (every consumer links
// Core; this module itself depends on it privately).
#include "HAL/Platform.h"

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Types.h>

/*
 * The capture-only script document (M4 Task 6) -- the ONE piece of VaCuus's own
 * code that lives inside the vendored library's module, and the location is
 * load-bearing, enforced by an observed SIGSEGV rather than by taste:
 *
 * RmlUi's custom RTTI compares the addresses of function-local statics inside
 * INLINE members (RMLUI_CUSTOM_RTTI, Traits.h:67-91). Under UBT's Linux modular
 * builds (-fvisibility-inlines-hidden), every .so that instantiates those
 * inline members gets its own copy of the static -- so an ElementDocument
 * subclass whose vtable is emitted in ANOTHER module answers
 * rmlui_dynamic_cast<ElementDocument*>() with NULL for every cast made from
 * inside VaCuusRml.so. Two such casts are load-bearing in the library itself:
 * the context constructor's cursor proxy -- instanced through the documents
 * base tag and cast with the assert compiled out, so the very next line writes
 * through null (Context.cpp:70-72; observed as a SIGSEGV at
 * null+offsetof(context) the first time a context was created with the
 * subclass registered from VaCuusJs) -- and Factory::InstanceDocumentStream,
 * which would refuse every document load (Factory.cpp:440-446). Defining the
 * class here, with its key function in this module's own .cpp, emits the
 * vtable -- and the inherited IsClass it dispatches -- in VaCuusRml.so, where
 * both casts resolve against the same static copy. (The mirror direction is
 * unchanged and documented at its site: VaCuusJs still must not
 * rmlui_dynamic_cast; it identifies these documents through the instancer's
 * membership set below.)
 */

/** One <head> script as the parser handed it over, in document order. */
struct FVaCuusCapturedScript
{
	/** True for <script>text</script>; false for <script src="...">. */
	bool bIsInline = false;

	/**
	 * True when this external script is an ES module (M4 Task 7, spec 3.7).
	 *
	 * THE SURFACE IS A NAMING CONVENTION -- src ends in `.mjs` -- AND CANNOT BE
	 * `<script type="module">`, because the type attribute never survives the
	 * parse: the head handler reads ONLY `src` from a <script> tag's attributes
	 * (XMLNodeHandlerHead.cpp:84-92) and DocumentHeader::Resource has no
	 * attribute channel at all (path/content/is_inline/line, DocumentHeader.h),
	 * so by the time LoadExternalScript fires the attribute is gone. Documented
	 * rather than fought: forking the vendored handler to carry one attribute
	 * would put VaCuus-side behavior inside the library diff. Consequences,
	 * stated plainly: inline <head> scripts are ALWAYS classic (no inline
	 * modules), and a `.js` src is always classic too -- import it FROM an
	 * `.mjs` entry instead (imports are modules by definition, whatever their
	 * extension; only the ENTRY is gated by the convention).
	 *
	 * ENTRIES DO NOT DEDUPE AGAINST THE MODULE CACHE, unlike imports: an `.mjs`
	 * entry whose module was already loaded as somebody's import -- or the same
	 * entry listed twice in one <head> -- executes a SECOND time with a second
	 * module instance, because JS_Eval's TYPE_MODULE path registers a fresh def
	 * without consulting the loaded-modules list (js_create_module,
	 * quickjs.c:29652) -- only the import-resolution path checks it first
	 * (js_host_resolve_imported_module, quickjs.c:30031-30036). Browsers dedupe
	 * entries by URL; we do not. Consequence: give an entry module no
	 * observable side effects you are not prepared to see twice, or make it an
	 * import behind a one-line entry.
	 */
	bool bIsModule = false;

	/** Inline only: the script text. */
	Rml::String Content;

	/**
	 * Inline: the DOCUMENT's source URL (the parser's own URL, stored by
	 * MakeInlineResource, XMLNodeHandlerHead.cpp:21-29, from the capture at
	 * :126-130). External: the raw src attribute -- resolved by the consumer at
	 * EXECUTION time, not capture time.
	 */
	Rml::String SourcePath;

	/** Inline only: 1-based line the script text starts on. */
	int SourceLine = 0;
};

/**
 * The document class every VaCuus view instances (M4 Task 6): capture-only
 * overrides of the two script hooks RmlUi reserves for "script plugins"
 * (declarations ElementDocument.h:106-113; base impls empty,
 * ElementDocument.cpp:461-463).
 *
 * CAPTURE, NEVER EXECUTE -- the spec 2(f) contract, and v1's recorded bug
 * (spec 12.1) is exactly the code this comment forbids. These hooks fire from
 * ProcessHeader when </head> closes (XMLNodeHandlerHead.cpp:98-110), which is
 * MID-PARSE: <body> does not exist yet and the document has not joined its
 * context. Worse, on a document REPLACE the whole load runs while the OLD
 * document -- and therefore the OLD JS context -- is still current
 * (Context::LoadDocument fires its plugin hooks inside itself, Context.cpp:299;
 * the old document closes only after it returns). Anything executed from here
 * lands in a context the recycle is about to free: JS silently dead after
 * every live reload. So the overrides store; the script host's OnDocumentReady
 * -- invoked by the document host after old-close and Show() -- runs what was
 * stored, in document order, in the fresh context.
 *
 * Every method is defined in the .cpp: the KEY FUNCTION -- the class's FIRST
 * out-of-line, non-pure virtual in DECLARATION order, which under the Itanium
 * ABI is the DESTRUCTOR, declared above both overrides -- is what pins the
 * vtable to this module (the file comment's whole point). Keeping the other
 * virtuals out-of-line too costs nothing and keeps the property immune to a
 * reordering edit.
 */
class VACUUSRML_API FVaCuusScriptDocument final : public Rml::ElementDocument
{
public:
	explicit FVaCuusScriptDocument(const Rml::String& Tag);
	virtual ~FVaCuusScriptDocument() override;

	const Rml::Vector<FVaCuusCapturedScript>& GetCapturedScripts() const { return CapturedScripts; }

protected:
	//~ Begin Rml::ElementDocument
	virtual void LoadInlineScript(const Rml::String& Content, const Rml::String& SourcePath, int SourceLine) override;
	virtual void LoadExternalScript(const Rml::String& SourcePath) override;
	//~ End Rml::ElementDocument

private:
	/** In document order -- one vector in the header, walked in order (ElementDocument.cpp:217-228). */
	Rml::Vector<FVaCuusCapturedScript> CapturedScripts;
};

/**
 * The instancer that makes every document an FVaCuusScriptDocument. RmlUi has
 * no separate "document instancer": Context::LoadDocument instances its base
 * tag ("body" by default, Context.h:303) through the ordinary element-instancer
 * map (Factory::InstanceDocumentStream, Factory.cpp:429-436), where
 * Factory::Initialise registers ElementInstancerGeneric<ElementDocument> under
 * "body" (Factory.cpp:180). RegisterWithFactory() replaces that default.
 *
 * PROCESS-IMMORTAL (one instance behind Get(), defined in the .cpp so exactly
 * one exists across every module): an element releases through THE INSTANCER
 * THAT MADE IT (Element::Release, Element.cpp:2168-2174), and documents die
 * AFTER the script host does -- the UI thread's Exit destroys the host first,
 * then closes contexts, and the context teardown is what actually frees the
 * unloaded documents. A host-owned instancer would be a dangling pointer under
 * every one of those frees.
 *
 * REGISTRATION timing is the CALLER's problem and the reason RegisterWithFactory
 * is a separate entry: Factory's instancer map exists only between
 * Rml::Initialise and Rml::Shutdown (a ControlledLifetimeResource,
 * Factory.cpp:141), so the script host registers from its Rml plugin's
 * OnInitialise -- exact on both orderings (immediate when RmlUi is already up,
 * Core.cpp:353-359; from NotifyInitialise, after Factory::Initialise installed
 * the default, on a later boot, Core.cpp:143, :155). No unregistration:
 * Factory::Shutdown drops the whole map, and a re-Initialise re-runs the hook.
 */
class VACUUSRML_API FVaCuusScriptDocumentInstancer final : public Rml::ElementInstancer
{
public:
	static FVaCuusScriptDocumentInstancer& Get();

	/** Registers this instancer under the documents base tag. RmlUi must be up (see the class comment). */
	static void RegisterWithFactory();

	/**
	 * True when Document was instanced HERE -- the license for a consumer's
	 * static_cast to FVaCuusScriptDocument. A membership set rather than
	 * rmlui_dynamic_cast, because the consumer sits in another module (the file
	 * comment's visibility argument, mirror direction), and rather than trusting
	 * "every document is ours by construction": a host created into an
	 * already-running RmlUi session cannot vouch for documents loaded before it
	 * existed. Un-mutexed like RmlUi's own instancer state -- instancing and
	 * release both happen on the one thread that drives the library.
	 */
	bool IsOurs(const Rml::ElementDocument* Document) const;

	/** TEST OBSERVABILITY: live documents instanced here (doubles as a leak gauge). */
	int GetNumLiveDocuments() const;

	//~ Begin Rml::ElementInstancer
	virtual Rml::ElementPtr InstanceElement(
		Rml::Element* Parent, const Rml::String& Tag, const Rml::XMLAttributes& Attributes) override;
	virtual void ReleaseElement(Rml::Element* Element) override;
	//~ End Rml::ElementInstancer

private:
	FVaCuusScriptDocumentInstancer();

	/** Backs IsOurs(); entries live exactly from InstanceElement to ReleaseElement. */
	Rml::UnorderedSet<const Rml::ElementDocument*> LiveDocuments;
};
