// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusScriptDocument.h"

#include <RmlUi/Core/Factory.h>

FVaCuusScriptDocument::FVaCuusScriptDocument(const Rml::String& Tag)
	: Rml::ElementDocument(Tag)
{
}

FVaCuusScriptDocument::~FVaCuusScriptDocument() = default;

void FVaCuusScriptDocument::LoadInlineScript(const Rml::String& Content, const Rml::String& SourcePath, int SourceLine)
{
	// Capture only -- executing here is v1's recorded bug (spec 12.1); the class
	// comment carries the full timing argument.
	FVaCuusCapturedScript& Script = CapturedScripts.emplace_back();
	Script.bIsInline = true;
	Script.Content = Content;
	Script.SourcePath = SourcePath;
	Script.SourceLine = SourceLine;
}

void FVaCuusScriptDocument::LoadExternalScript(const Rml::String& SourcePath)
{
	FVaCuusCapturedScript& Script = CapturedScripts.emplace_back();
	Script.bIsInline = false;
	Script.SourcePath = SourcePath;

	// The `.mjs` naming convention (FVaCuusCapturedScript::bIsModule has the
	// why-not-type-attribute argument). Sniffed HERE, at capture, so the decision
	// sits next to the struct that documents it. The captured path is the head
	// handler's joined-and-pipe-encoded form (':' -> '|', Absolutepath,
	// XMLNodeHandlerHead.cpp:14-19), which cannot touch the extension -- an
	// extension with a colon in it was never a file the VFS could resolve.
	const size_t Length = SourcePath.size();
	if (Length >= 4)
	{
		const char* Tail = SourcePath.c_str() + Length - 4;
		Script.bIsModule = Tail[0] == '.' && (Tail[1] == 'm' || Tail[1] == 'M') && (Tail[2] == 'j' || Tail[2] == 'J') &&
						   (Tail[3] == 's' || Tail[3] == 'S');
	}
}

FVaCuusScriptDocumentInstancer::FVaCuusScriptDocumentInstancer() = default;

FVaCuusScriptDocumentInstancer& FVaCuusScriptDocumentInstancer::Get()
{
	// Function-local static in a NON-INLINE function: exactly one instance in
	// the process, whichever module asks (an inline Get() would mint one per
	// .so under -fvisibility-inlines-hidden -- the same duplication the file
	// comment exists to defeat). Process-immortal by the release-path argument
	// in the class comment.
	static FVaCuusScriptDocumentInstancer Instancer;
	return Instancer;
}

void FVaCuusScriptDocumentInstancer::RegisterWithFactory()
{
	// "body" is the documents base tag every context starts with (member
	// default, Context.h:303; VaCuus never calls SetDocumentsBaseTag).
	// RegisterElementInstancer lowercases the key itself (Factory.cpp:295-298),
	// and re-registering an existing name simply overwrites the map slot --
	// which is exactly how this replaces the default (Factory.cpp:180) and how
	// a second registration stays idempotent. A side effect worth knowing: a
	// context's CURSOR PROXY is instanced through the base tag too
	// (Context.cpp:69), so every context construction mints one inert
	// FVaCuusScriptDocument that only ever captures nothing.
	Rml::Factory::RegisterElementInstancer("body", &Get());
}

bool FVaCuusScriptDocumentInstancer::IsOurs(const Rml::ElementDocument* Document) const
{
	return LiveDocuments.count(Document) > 0;
}

int FVaCuusScriptDocumentInstancer::GetNumLiveDocuments() const
{
	return static_cast<int>(LiveDocuments.size());
}

Rml::ElementPtr FVaCuusScriptDocumentInstancer::InstanceElement(
	Rml::Element* /*Parent*/, const Rml::String& Tag, const Rml::XMLAttributes& /*Attributes*/)
{
	// The ElementInstancerGeneric shape (ElementInstancer.h:72-88): plain new,
	// released below with plain delete. The tag is forwarded so a nested <body>
	// inside a template keeps its parsed tag, same as the default instancer.
	FVaCuusScriptDocument* Document = new FVaCuusScriptDocument(Tag);
	LiveDocuments.insert(Document);
	return Rml::ElementPtr(Document);
}

void FVaCuusScriptDocumentInstancer::ReleaseElement(Rml::Element* Element)
{
	// The pairing contract ("the same instancer that allocated the element
	// releases it", ElementInstancer.h:14-19) is what licenses this cast: only
	// InstanceElement above ever set this object as an element's instancer.
	FVaCuusScriptDocument* Document = static_cast<FVaCuusScriptDocument*>(Element);
	LiveDocuments.erase(Document);
	delete Document;
}
