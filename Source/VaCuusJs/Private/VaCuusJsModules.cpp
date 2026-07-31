// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

/*
 * ES modules over the VFS (M4 Task 7, spec 3.7): the normalize/loader pair
 * behind JS_SetModuleLoaderFunc (installed per runtime from the view-context
 * constructor), the shared name canonicalizer, and the view context's module
 * ENTRY eval. The drain-then-inspect half -- TLA refusal, rejection routing --
 * is the script host's (FVaCuusJsScriptHost::EvalModule), because the bounded
 * job drain lives there (spec 3.5).
 */

#include "VaCuusJsModules.h"

#include "VaCuusContentPaths.h"
#include "VaCuusJs.h"
#include "VaCuusJsScriptSource.h"
#include "VaCuusJsViewContext.h"

#include "Misc/Paths.h"

namespace VaCuusJsModules
{
FString StripVfsScheme(const FString& InName)
{
	return InName.StartsWith(VfsScheme, ESearchCase::IgnoreCase) ? InName.Mid(FCString::Strlen(VfsScheme)) : InName;
}

bool CanonicalizeVfsRelativePath(const FString& InPath, FString& OutCanonical)
{
	// '\' -> '/' first: specifiers in JS source use '/', but an entry name can
	// carry a platform separator if a document's src attribute did.
	FString Unified = InPath.Replace(TEXT("\\"), TEXT("/"));

	TArray<FString> Segments;
	TArray<FString> Stack;
	Unified.ParseIntoArray(Segments, TEXT("/"), /*CullEmpty=*/true);
	for (FString& Segment : Segments)
	{
		if (Segment == TEXT("."))
		{
			continue;
		}
		if (Segment == TEXT(".."))
		{
			if (Stack.IsEmpty())
			{
				// A climb above the VFS root: "<Root>/../x" would resolve OUTSIDE
				// the document roots. Refused here, once, for every caller.
				return false;
			}
			Stack.Pop();
			continue;
		}
		Stack.Add(MoveTemp(Segment));
	}
	OutCanonical = FString::Join(Stack, TEXT("/"));
	return !OutCanonical.IsEmpty();
}

FString MakeModuleName(const FString& InRootRelativePath)
{
	FString Canonical;
	if (!CanonicalizeVfsRelativePath(StripVfsScheme(InRootRelativePath), Canonical))
	{
		// See the header: a bad path becomes a loadable-looking name whose load
		// FAILS LOUDLY (the loader's miss Error names it), never a silent drop.
		Canonical = StripVfsScheme(InRootRelativePath);
	}
	return VfsScheme + Canonical;
}
}	 // namespace VaCuusJsModules

namespace VaCuusJsModulesInternal
{
/**
 * import.meta.url = the module's canonical vfs name -- the same string the
 * per-context cache keys on, so a module can hand its own url back to import()
 * and get ITSELF. JS_GetImportMeta lazily mints a null-proto object the host
 * populates (quickjs.h:1193-1194, quickjs.c:30920-30932); C_W_E matches the
 * web's configurable-writable-enumerable url property. Failure (OOM at the
 * cap) loses the property, not the module -- clear and continue.
 */
static void SetImportMetaUrl(JSContext* Ctx, JSModuleDef* Module, const char* NameUtf8)
{
	JSValue Meta = JS_GetImportMeta(Ctx, Module);
	if (JS_IsException(Meta))
	{
		JS_FreeValue(Ctx, JS_GetException(Ctx));
		return;
	}
	JS_DefinePropertyValueStr(Ctx, Meta, "url", JS_NewString(Ctx, NameUtf8), JS_PROP_C_W_E);
	JS_FreeValue(Ctx, Meta);
}
}	 // namespace VaCuusJsModulesInternal

char* FVaCuusJsViewContext::ModuleNormalizeThunk(
	JSContext* Ctx, const char* BaseName, const char* Name, void* /*Opaque*/)
{
	using namespace VaCuusJsModules;

	FVaCuusJsViewContext* Self = GetSelfOrNull(Ctx);
	if (Self == nullptr)
	{
		// The dead-context rule: a removed view's pinned job running a dynamic
		// import() must not resolve modules into a dead world. NULL-with-exception
		// is this callback's contractual failure shape (quickjs.h:1154-1155).
		JS_ThrowReferenceError(Ctx, "module '%s' requested on a dead view context", Name);
		return nullptr;
	}

	const FString Specifier(UTF8_TO_TCHAR(Name));
	FString Combined;
	if (Specifier.StartsWith(TEXT("./")) || Specifier.StartsWith(TEXT("../")))
	{
		// Relative to the IMPORTER's directory. BaseName is the importer's own
		// canonical name for a module-to-module import (the engine passes the
		// importer's module_name -- what it was compiled as, and both compile
		// sites here compile under the canonical vfs name). For a dynamic
		// import() from a CLASSIC script the base is that script's SOURCE NAME,
		// and what happens depends on its shape: a slash-free name ("exec-fifo",
		// an ExecuteScript label) has directory "" and the specifier resolves at
		// the VFS root; an INLINE script's name ("vacuus://x.rml:3") contains
		// slashes, so GetPath answers "vacuus:/" -- StripVfsScheme strips only
		// vfs:// -- and the combined name canonicalizes to "vacuus:/<spec>",
		// which resolves under no root and fails LOUDLY at the loader, naming
		// the probe. So: relative dynamic imports work from named classic
		// sources, and from inline documents they are a diagnosed miss -- use a
		// root-relative or vfs:// specifier there. Documented, not fought.
		const FString BaseDir = FPaths::GetPath(StripVfsScheme(FString(UTF8_TO_TCHAR(BaseName))));
		Combined = BaseDir.IsEmpty() ? Specifier : BaseDir / Specifier;
	}
	else
	{
		// Bare and vfs://-prefixed specifiers are root-relative; the strip is
		// CanonicalizeVfsRelativePath's caller's job via StripVfsScheme in
		// MakeModuleName's shape below.
		Combined = Specifier;
	}

	FString Canonical;
	if (!CanonicalizeVfsRelativePath(StripVfsScheme(Combined), Canonical))
	{
		UE_LOG(LogVaCuusJS, Error,
			TEXT("View %u: module specifier '%hs' (from '%hs') escapes the document roots; refused"),
			Self->GetViewId(), Name, BaseName);
		JS_ThrowReferenceError(Ctx, "module specifier '%s' escapes the document roots", Name);
		return nullptr;
	}
	const FString ModuleName = VfsScheme + Canonical;

	// The contract: js_malloc'd (quickjs.h:1154-1155) -- the engine js_free()s it
	// on every exit of js_host_resolve_imported_module (quickjs.c:30026, :30033,
	// :30046, :30055). js_malloc throws OOM itself on failure, so bare NULL is
	// already NULL-with-exception.
	const FTCHARToUTF8 NameUtf8(*ModuleName);
	char* Result = static_cast<char*>(js_malloc(Ctx, NameUtf8.Length() + 1));
	if (Result != nullptr)
	{
		FMemory::Memcpy(Result, NameUtf8.Get(), NameUtf8.Length());
		Result[NameUtf8.Length()] = '\0';
	}
	return Result;
}

JSModuleDef* FVaCuusJsViewContext::ModuleLoaderThunk(JSContext* Ctx, const char* ModuleName, void* /*Opaque*/)
{
	using namespace VaCuusJsModules;

	FVaCuusJsViewContext* Self = GetSelfOrNull(Ctx);
	if (Self == nullptr)
	{
		JS_ThrowReferenceError(Ctx, "module '%s' loaded on a dead view context", ModuleName);
		return nullptr;
	}

	// ModuleName is normalize's output: "vfs://<canonical>". Strip the scheme
	// BEFORE resolution -- FPaths::IsRelative calls "vfs://x" relative, so the
	// unstripped name would probe "<Root>/vfs://x" and miss (the
	// plugin-integration.md section 3 trap) -- then let the ordered roots (or an
	// absolute passthrough) answer, and read through the pak-transparent
	// IPlatformFile path, same as <script src> (VaCuusJsScriptSource).
	const FString VfsPath = StripVfsScheme(FString(UTF8_TO_TCHAR(ModuleName)));
	const FString Resolved = VaCuusContentPaths::ResolveExistingDocument(VfsPath);
	FString Source;
	if (Resolved.IsEmpty() || !VaCuusJsScriptSource::ReadScriptFile(Resolved, Source))
	{
		// Both diagnostics (spec 3.7): OUR Error names what was probed and where;
		// the thrown ReferenceError -- the engine's own no-loader wording
		// (quickjs.c:30044) -- rides the pending-exception channel to the
		// importer, where the eval error path logs and counts it. The engine adds
		// nothing on a NULL return from an INSTALLED loader (quickjs.c:30051-30057
		// just propagates), so the throw here is what keeps the script-visible
		// shape identical either way.
		UE_LOG(LogVaCuusJS, Error,
			TEXT("View %u: module '%hs' did not resolve to a readable file under the DevUI roots (probed '%s'); the ")
			TEXT("importer gets a ReferenceError"),
			Self->GetViewId(), ModuleName, *VfsPath);
		JS_ThrowReferenceError(Ctx, "could not load module '%s'", ModuleName);
		return nullptr;
	}

	// Compile UNDER THE CANONICAL NAME -- the eval filename becomes
	// m->module_name, which is the BaseName the normalize thunk sees for this
	// module's own imports; compiling under the disk path would break every
	// relative chain. Static imports of THIS module resolve inside this call
	// (quickjs.c:37395-37404), recursing through this very thunk. A compile or
	// nested-resolve failure returns NULL with the exception pending -- the
	// importer's diagnostic, not ours to duplicate. No entry guard here: the
	// loader only ever runs INSIDE an eval or a job drain, both already guarded,
	// and a nested guard arms nothing by design (FVaCuusJsEntryGuard).
	const FTCHARToUTF8 SourceUtf8(*Source);
	const JSValue Compiled =
		JS_Eval(Ctx, SourceUtf8.Get(), SourceUtf8.Length(), ModuleName, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
	if (JS_IsException(Compiled))
	{
		return nullptr;
	}

	// A COMPILE_ONLY module eval yields a JS_TAG_MODULE value whose pointer is
	// the JSModuleDef (quickjs.h:440-444, :817-820). The def is already on
	// ctx->loaded_modules (quickjs.c:29652) -- referenced by the context, so the
	// VALUE can be freed and the borrowed pointer returned: the canonical
	// embedder pattern (the notes' section 7), and the same borrow the engine's
	// own cache-hit path hands back (quickjs.c:30031-30036).
	JSModuleDef* Module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(Compiled));
	VaCuusJsModulesInternal::SetImportMetaUrl(Ctx, Module, ModuleName);
	JS_FreeValue(Ctx, Compiled);
	return Module;
}

JSValue FVaCuusJsViewContext::EvalModuleToPromise(const FString& Source, const FString& ModuleName)
{
	if (Ctx == nullptr)
	{
		return JS_UNDEFINED;
	}

	const FTCHARToUTF8 SourceUtf8(*Source);
	const FTCHARToUTF8 NameUtf8(*ModuleName);

	// Phase 1, compile+resolve: every static import in the graph loads here
	// (quickjs.c:37395-37404 -- js_resolve_module runs BEFORE the COMPILE_ONLY
	// return), so a missing import surfaces from THIS call as the loader's
	// ReferenceError. Guarded and consumed in the house Eval shape
	// (VaCuusJsRuntime.h's usage contract).
	JSValue Compiled;
	{
		FVaCuusJsEntryGuard Guard(Runtime, Ctx, *ModuleName);
		Compiled =
			JS_Eval(Ctx, SourceUtf8.Get(), SourceUtf8.Length(), NameUtf8.Get(), JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
	}
	if (JS_IsException(Compiled))
	{
		Runtime.ReportException(Ctx, *ModuleName);
		return JS_UNDEFINED;
	}

	// The entry gets its import.meta.url exactly like every imported module --
	// the loader stamps imports, this stamps the root, nobody is special.
	VaCuusJsModulesInternal::SetImportMetaUrl(
		Ctx, static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(Compiled)), NameUtf8.Get());

	// Phase 2, evaluate: JS_EvalFunction consumes Compiled (JS_FreeValue at
	// quickjs.c:37278) and runs the graph's bodies, dependencies first. The
	// return is the module promise, dup'd for us (quickjs.c:31553-31554, :31589).
	// A RUNTIME throw in a body is NOT an exception here -- it rejects the
	// promise (quickjs.c:31571-31575) and fires the rejection tracker with
	// is_handled=false (quickjs.c:54371-54375; TWICE for a throw before the
	// first await -- the engine's own sync path leaves the body's async-function
	// promise unhandled too, quickjs.c:31390-31410; the ThrowRejects test pins
	// it), which logs and counts it; the exception branch below is for
	// link-time failures only.
	JSValue Ret;
	{
		FVaCuusJsEntryGuard Guard(Runtime, Ctx, *ModuleName);
		Ret = JS_EvalFunction(Ctx, Compiled);
	}
	if (JS_IsException(Ret))
	{
		Runtime.ReportException(Ctx, *ModuleName);
		return JS_UNDEFINED;
	}
	return Ret;
}
