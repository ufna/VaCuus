// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "VaCuusJsDocumentTestHost.h"
#include "VaCuusJsDomTestRig.h"

#include "VaCuusContentPaths.h"
#include "VaCuusTranslation.h"

#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"

/*
 * M5 TASK 8 — @vacuus/preact, the typings manifest, the committed bundle, and
 * the localization hook (spec §2(l), §3.1). Five surfaces, one file:
 *
 *  - CONFORMANCE: the committed vacuus-api-manifest.json (extracted from
 *    vacuus.d.ts by `vacuus manifest`, staleness-checked by Web/smoke.mjs)
 *    walked against the REAL prototypes and globals BOTH WAYS — typed-but-absent
 *    fails, present-but-untyped fails. The manifest documents its exclusions.
 *  - ADAPTER CONTRACT: the committed @vacuus/preact fixture bundle
 *    (fixture-vacuus-preact.js) pins what the options-adapter fixes — onClick
 *    firing on RmlUi's lowercase "click", className landing as the class
 *    ATTRIBUTE. The broken halves are E-P2's recorded observations against
 *    stock preact (ep-observations.md): "Click" registered and never fired, a
 *    dead className attribute — the same facade, minus the adapter.
 *  - BUNDLE MOUNT + PROVENANCE: the committed Content/DevUI/M5Hud/hud_bundle.js
 *    loaded through the REAL captured-script path (script src resolved through
 *    the DevUI roots), DOM-probed; its provenance manifest checked against the
 *    current facade manifest, with the SKIP-WITH-NAMED-WARNING branch exercised
 *    on a doctored hash first (spec §2(l): a facade change must produce a
 *    rebuild instruction, not an undiagnosable red).
 *  - E-P6 / E-P7 (plan 8.2): desync observability and reload re-mount, run
 *    against the adapter bundle; outcomes recorded in ep-observations.md.
 *  - TRANSLATE: the vacuus.translate snapshot design — identity + params before
 *    any table, table lookup + substitution + replacement after, and the RML
 *    text half through the same snapshot (FVaCuusSystemInterface::TranslateString).
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsPreactConformanceTest, "VaCuus.Js.Preact.Conformance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsPreactAdapterContractTest, "VaCuus.Js.Preact.AdapterContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsPreactBundleMountTest, "VaCuus.Js.Preact.BundleMount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsPreactDesyncTest, "VaCuus.Js.Preact.DesyncObservability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsPreactReloadRemountTest, "VaCuus.Js.Preact.ReloadRemount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsTranslateTest, "VaCuus.Js.Translate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace VaCuusJsWebDxTest
{
using namespace VaCuusJsDomTest;
using namespace VaCuusJsDocumentTest;

/** Reads a committed file from the plugin's canonical DevUI root (the facade-test helper's shape). */
inline bool LoadDevUIFile(FAutomationTestBase& Test, const TCHAR* RelativePath, FString& OutText)
{
	const TArray<FString>& Roots = VaCuusContentPaths::GetDocumentRoots();
	if (Roots.IsEmpty())
	{
		Test.AddError(TEXT("no DevUI document roots exist"));
		return false;
	}
	const FString Path = Roots[0] / RelativePath;
	if (!FFileHelper::LoadFileToString(OutText, *Path))
	{
		Test.AddError(FString::Printf(TEXT("could not read the committed file '%s'"), *Path));
		return false;
	}
	return true;
}

/** The rig boot + document + bind dance (the facade-test helper, replicated — it is file-local there). */
inline bool BootWithDocument(FAutomationTestBase& Test, FDomTestRig& Rig, FDomProbeHost*& OutProbe, uint32& OutViewId,
	const TCHAR* Prefix, const TCHAR* Document)
{
	OutProbe = nullptr;
	OutViewId = Rig.AddViewWithDocument(OutProbe, Prefix, Document);

	bool bBound = false;
	FDomProbeHost* Probe = OutProbe;
	const uint32 ViewId = OutViewId;
	Rig.RunOnUI([&bBound, Probe, ViewId]()
		{
			Rml::ElementDocument* Document = Probe->GetDocument();
			bBound = Document != nullptr;
			FWrappedDomHost::Inner->BindDocumentForTest(ViewId, Document);
		});
	return Test.TestTrue(TEXT("the document loaded and bound"), bBound);
}

/**
 * SHA-1 hex (lowercase) over a file's raw bytes — the same digest
 * `vacuus build` records in the provenance manifest (Web/packages/cli/lib/
 * paths.mjs facadeManifestHash: node crypto sha1 over the identical bytes), so
 * the two sides agree or the provenance check is answering a real difference.
 */
inline bool HashFileSha1(const FString& Path, FString& OutHex)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		return false;
	}
	FSHAHash Hash;
	FSHA1::HashBuffer(Bytes.GetData(), Bytes.Num(), Hash.Hash);
	OutHex = Hash.ToString().ToLower();
	return true;
}

/** Minimal flat-JSON string field read — three known fields, no engine Json dependency. */
inline FString ExtractJsonString(const FString& Json, const TCHAR* Field)
{
	const FString Needle = FString::Printf(TEXT("\"%s\": \""), Field);
	int32 Start = Json.Find(Needle, ESearchCase::CaseSensitive);
	if (Start == INDEX_NONE)
	{
		return FString();
	}
	Start += Needle.Len();
	const int32 End = Json.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
	return End == INDEX_NONE ? FString() : Json.Mid(Start, End - Start);
}

/**
 * The provenance gate (spec §2(l)): true = provenance matches the CURRENT
 * facade manifest, run the bundle assertions; false = the caller returns green
 * WITHOUT them, and this has logged the named Warning telling the controller
 * exactly what to run. The caller registers the expectation for the warning
 * (or, in the doctored exercise, expects it deliberately).
 */
inline bool CheckProvenanceOrWarn(const FString& RecordedHash, const FString& CurrentHash, const TCHAR* BundleName)
{
	if (RecordedHash == CurrentHash)
	{
		return true;
	}
	UE_LOG(LogVaCuusJS, Warning,
		TEXT("VaCuus M5: %s provenance mismatch — the bundle was built against facade manifest %s but the current ")
		TEXT("manifest is %s. The bundle test is SKIPPED (not failed); rebuild and commit: ")
		TEXT("node Web/packages/cli/bin/vacuus.mjs build --app Web/apps/demo-hud"),
		BundleName, *RecordedHash, *CurrentHash);
	return false;
}

/**
 * The conformance walk (spec §2(l)), run inside the real context: BOTH WAYS
 * over every object the facade owns — element prototype, document prototype
 * (own members; its chain to the element prototype is itself asserted), a
 * classList instance, console, vacuus, the event prototype and a dispatched
 * event instance — plus typed->present over the globals (the manifest's
 * globalScan exclusion documents why present->typed cannot run on globalThis).
 * Completion value: 'clean' or ';'-joined problems.
 */
static const char* GConformanceWalk = R"JS(
(() => {
  const M = globalThis.VACUUS_MANIFEST.groups;
  const problems = [];
  const walkBoth = (group, obj, expected) => {
    const actual = Object.getOwnPropertyNames(obj).sort();
    for (const n of expected) if (!actual.includes(n)) problems.push(group + ': typed-but-absent ' + n);
    for (const n of actual) if (!expected.includes(n)) problems.push(group + ': present-but-untyped ' + n);
  };
  const el = document.createElement('div');
  const elProto = Object.getPrototypeOf(el);
  const docProto = Object.getPrototypeOf(document);
  walkBoth('element', elProto, M.element);
  walkBoth('document', docProto, M.document);
  if (Object.getPrototypeOf(docProto) !== elProto) problems.push('document proto does not chain to element proto');
  const text = document.createTextNode('t');
  if (Object.getPrototypeOf(text) !== elProto) problems.push('text-node proto is not the element proto');
  walkBoth('classList', el.classList, M.classList);
  walkBoth('console', console, M.console);
  walkBoth('vacuus', vacuus, M.vacuus);
  let ev = null;
  const mount = document.getElementById('mount');
  mount.addEventListener('probe', (e) => { ev = e; });
  mount.dispatchEvent('probe');
  if (!ev) problems.push('event: dispatch never delivered');
  else {
    walkBoth('eventPrototype', Object.getPrototypeOf(ev), M.eventPrototype);
    walkBoth('eventInstance', ev, M.eventInstance);
  }
  for (const n of M.globals) {
    if (!(n in globalThis)) problems.push('globals: typed-but-absent ' + n);
  }
  return problems.join('; ') || 'clean';
})()
)JS";

static const TCHAR* GMountDocument = TEXT(R"(<rml>
<head><style>body { display: block; font-family: LatoLatin; font-size: 14px; } div { display: block; } button { display: inline-block; } h1 { display: block; } span { display: inline; }</style></head>
<body><div id="mount"/></body>
</rml>)");
}	 // namespace VaCuusJsWebDxTest

/**
 * The manifest, walked both ways against the running engine. Provenance chain:
 * vacuus.d.ts -> `vacuus manifest` -> the committed JSON (Web/smoke.mjs fails
 * when stale) -> this walk (fails when the engine and the manifest disagree in
 * either direction). RESTORE-THE-BUG (run for the task log, both directions):
 * deleting 'translate' from the committed manifest's vacuus group fails
 * present-but-untyped; adding a fictional 'frobnicate' fails typed-but-absent.
 */
bool FVaCuusJsPreactConformanceTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsWebDxTest;

	FString Manifest;
	if (!LoadDevUIFile(*this, TEXT("Tests/vacuus-api-manifest.json"), Manifest))
	{
		return false;
	}

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	FDomProbeHost* Probe = nullptr;
	uint32 ViewId = 0;
	if (!BootWithDocument(*this, Rig, Probe, ViewId, TEXT("vacuus_js_conformance"), GMountDocument))
	{
		return false;
	}

	// JSON is a valid JS expression; the parenthesized eval installs it verbatim.
	const FString Install = FString::Printf(TEXT("globalThis.VACUUS_MANIFEST = (%s); 'installed'"), *Manifest);
	FString InstallResult;
	Rig.RunOnUI([&InstallResult, ViewId, &Install]()
		{ InstallResult = EvalString(*FWrappedDomHost::Inner, ViewId, TCHAR_TO_UTF8(*Install)); });
	if (!TestEqual(TEXT("the manifest installed into the context"), InstallResult, FString(TEXT("installed"))))
	{
		return false;
	}

	TestEqual(TEXT("the both-ways walk is clean"), Rig.Eval(ViewId, GConformanceWalk), FString(TEXT("clean")));

	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

/**
 * The adapter's two E-P2 fixes, end to end through the committed
 * fixture-vacuus-preact.js (@vacuus/preact = stock preact + the options
 * adapter): a dispatched lowercase "click" — the only case RmlUi ever fires —
 * reaches the onClick handler and commits its setState in the same frame's job
 * drain; className renders as the class ATTRIBUTE RCSS matches. The stock
 * halves of both (dead "Click" registration, dead className attribute) are
 * E-P2's recorded observations, ep-observations.md.
 */
bool FVaCuusJsPreactAdapterContractTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsWebDxTest;

	FString Bundle;
	if (!LoadDevUIFile(*this, TEXT("Tests/fixture-vacuus-preact.js"), Bundle))
	{
		return false;
	}

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	FDomProbeHost* Probe = nullptr;
	uint32 ViewId = 0;
	if (!BootWithDocument(*this, Rig, Probe, ViewId, TEXT("vacuus_js_adapter"), GMountDocument))
	{
		return false;
	}

	FString EvalResult;
	Rig.RunOnUI([&EvalResult, ViewId, &Bundle]()
		{ EvalResult = EvalString(*FWrappedDomHost::Inner, ViewId, TCHAR_TO_UTF8(*Bundle)); });

	TestEqual(TEXT("the adapter bundle mounted"),
		Rig.Eval(ViewId,
			"[globalThis.RUNS, document.getElementById('probe-root') !== null,"
			" document.getElementById('probe-btn').innerRML,"
			" document.getElementById('probe-rows').children.length].map(String).join('|')"),
		FString(TEXT("1|true|n:0|2")));

	// The event fix: RmlUi's own lowercase type fires the TSX onClick. The
	// setState commits in this frame's job drain (the M4 pump order), so the
	// next eval reads the updated text.
	Rig.Eval(ViewId, "document.getElementById('probe-btn').dispatchEvent('click'); 'ok'");
	TestEqual(TEXT("onClick fired on lowercase 'click' and committed"),
		Rig.Eval(ViewId, "document.getElementById('probe-btn').innerRML"), FString(TEXT("n:1")));

	// The className fix: the attribute RCSS matches, not a dead 'className'.
	TestEqual(TEXT("className landed as the class attribute"),
		Rig.Eval(ViewId,
			"const cn = document.getElementById('probe-cn');"
			"[cn.getAttribute('class'), cn.getAttribute('className'), cn.classList.contains('via-class-name')]"
			".map(String).join('|')"),
		FString(TEXT("via-class-name|null|true")));

	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

/**
 * The committed demo bundle (spec §2(l), §8): provenance gate first — the
 * doctored-hash branch exercised deliberately (the named skip warning IS the
 * contract), then the real gate — then the bundle through the REAL captured
 * <script src> path (FJsDocProbeHost runs OnDocumentReady exactly where the
 * production host does; src resolves through the DevUI roots), DOM-probed.
 */
bool FVaCuusJsPreactBundleMountTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsWebDxTest;

	const TArray<FString>& Roots = VaCuusContentPaths::GetDocumentRoots();
	if (!TestTrue(TEXT("a DevUI root exists"), !Roots.IsEmpty()))
	{
		return false;
	}

	FString Provenance;
	if (!LoadDevUIFile(*this, TEXT("M5Hud/hud_bundle.provenance.json"), Provenance))
	{
		return false;
	}
	const FString RecordedHash = ExtractJsonString(Provenance, TEXT("facadeManifestHash"));
	TestTrue(TEXT("the provenance records a facade-manifest hash"), RecordedHash.Len() == 40);

	FString CurrentHash;
	if (!TestTrue(TEXT("the committed facade manifest hashes"),
			HashFileSha1(Roots[0] / TEXT("Tests/vacuus-api-manifest.json"), CurrentHash)))
	{
		return false;
	}

	// The skip branch, exercised on a doctored hash so the named warning has
	// been SEEN to fire (an invariant with no observable rots): expected, then
	// provoked, then asserted refused. The self-check names itself so its
	// expectation cannot swallow a REAL mismatch warning below.
	AddExpectedMessagePlain(TEXT("hud_bundle.js (self-check) provenance mismatch"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	TestFalse(TEXT("a doctored provenance is refused with the named warning"),
		CheckProvenanceOrWarn(
			TEXT("0000000000000000000000000000000000000000"), CurrentHash, TEXT("hud_bundle.js (self-check)")));

	// The real gate. A mismatch here is the legitimate skip: the facade moved
	// since the bundle was built, and the named warning says exactly what to
	// rebuild — expected FIRST (the comparison is re-run quietly to decide),
	// so the suite stays green-with-signal, never red (spec §2(l)).
	if (RecordedHash != CurrentHash)
	{
		AddExpectedMessagePlain(TEXT("hud_bundle.js provenance mismatch"), ELogVerbosity::Warning,
			EAutomationExpectedMessageFlags::Contains, 1);
		CheckProvenanceOrWarn(RecordedHash, CurrentHash, TEXT("hud_bundle.js"));
		AddInfo(TEXT("hud_bundle.js provenance mismatch: bundle assertions SKIPPED (the warning carries the rebuild command)"));
		return true;
	}

	// The demo bundle reads 'hud'.Health per rAF frame; no game feeds it here,
	// so the read surface latches exactly one Warning (the M4 cost test's shape).
	AddExpectedMessagePlain(TEXT("model 'hud', path 'Health'"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	// The real path: captured <script src>, resolved through the DevUI roots at
	// OnDocumentReady — the committed bundle, not a copy of its text.
	static const TCHAR* GBundleDocument = TEXT(R"(<rml>
<head><style>body { display: block; font-family: LatoLatin; font-size: 14px; } div { display: block; } button { display: inline-block; } h1 { display: block; } span { display: inline; }</style>
<script src="M5Hud/hud_bundle.js"></script>
</head>
<body><div id="mount"/></body>
</rml>)");

	FJsDocProbeHost* Probe = nullptr;
	const uint32 ViewId = AddSeamViewWithDocument(Rig, Probe, TEXT("vacuus_js_bundle"), GBundleDocument);
	if (!TestTrue(TEXT("frames pumped"), PumpRealFrames(*Rig.Thread, 3)))
	{
		return false;
	}

	// MOUNTS AND RENDERS (the DOM probe): the deterministic parts — root, the
	// translated-identity title, the two seeded killfeed rows (>= 2: the 1.5 s
	// beat may legally have run under a slow pump), a numeric health readout.
	FString Mounted;
	Rig.RunOnUI([&Mounted, ViewId]()
		{
			Mounted = EvalString(*FWrappedDomHost::Inner, ViewId,
				"[document.getElementById('hud-root') !== null,"
				" document.getElementById('hud-title').innerRML,"
				" document.getElementById('killfeed').children.length >= 2,"
				" /^\\d+$/.test(document.getElementById('health-val').innerRML)].map(String).join('|')");
		});
	TestEqual(TEXT("the committed bundle mounted and rendered"), Mounted,
		FString(TEXT("true|VaCuus M5 // TSX HUD|true|true")));

	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

/**
 * E-P6 — desync observability (preact-contract.md §7, spec §2(k)): a C++
 * SetInnerRML under a live preact tree, then setState. The outcome this test
 * PINS (and ep-observations.md records): the divergence is TOTAL AND TOTALLY
 * SILENT — preact's text writes land on dead wrappers as no-ops, its
 * re-inserts on dead parents refuse as nulls, the document shows nothing, and
 * not one JS error or refused-op diagnostic exists anywhere (G15's never-throw
 * contract working exactly as designed, against the app). The refused-op
 * counter G15 priced remains future work; this test is the evidence for it.
 */
bool FVaCuusJsPreactDesyncTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsWebDxTest;

	FString Bundle;
	if (!LoadDevUIFile(*this, TEXT("Tests/fixture-vacuus-preact.js"), Bundle))
	{
		return false;
	}

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	FDomProbeHost* Probe = nullptr;
	uint32 ViewId = 0;
	if (!BootWithDocument(*this, Rig, Probe, ViewId, TEXT("vacuus_js_desync"), GMountDocument))
	{
		return false;
	}

	FString EvalResult;
	Rig.RunOnUI([&EvalResult, ViewId, &Bundle]()
		{ EvalResult = EvalString(*FWrappedDomHost::Inner, ViewId, TCHAR_TO_UTF8(*Bundle)); });
	if (!TestEqual(TEXT("mounted"), Rig.Eval(ViewId, "document.getElementById('probe-root') !== null ? 'yes' : 'no'"),
			FString(TEXT("yes"))))
	{
		return false;
	}

	// THE C++ SURGERY: kill the preact-owned subtree under the app's feet — the
	// exact hazard class game code can produce any time (G15).
	Rig.RunOnUI([Probe]()
		{
			Rml::Element* Mount = Probe->GetDocument()->GetElementById("mount");
			Mount->SetInnerRML("");
		});

	const uint64 ErrorsBefore = FWrappedDomHost::Inner->GetRuntime()->GetNumErrors();

	// setState against the dead tree: a text bump AND a keyed-list append.
	Rig.Eval(ViewId, "globalThis.bump(); globalThis.pushRow('r2'); 'ok'");

	// The observation, pinned: nothing re-appears, nothing throws, nothing logs.
	TestEqual(TEXT("E-P6: the desync is total — the mount stays empty, lookups answer null"),
		Rig.Eval(ViewId,
			"[document.getElementById('mount').innerRML === '',"
			" document.getElementById('probe-root'), document.getElementById('probe-btn')].map(String).join('|')"),
		FString(TEXT("true|null|null")));
	TestEqual(TEXT("E-P6: zero JS errors — the never-throw contract makes the divergence silent"),
		FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), ErrorsBefore);

	return true;
}

/**
 * E-P7 — reload re-mount (preact-contract.md §5/§7, spec §2(k)): the document
 * recycle destroys the whole JS context, the bundle re-runs from module top
 * level against the fresh document, and nothing leaks — listener refs return
 * to zero once no script re-registers them. Driven through the REAL seam
 * (FJsDocProbeHost mirrors the production AdoptDocument/CloseDocument order).
 */
bool FVaCuusJsPreactReloadRemountTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsWebDxTest;

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	static const TCHAR* GScriptedDocument = TEXT(R"(<rml>
<head><style>body { display: block; font-family: LatoLatin; font-size: 14px; } div { display: block; } button { display: inline-block; }</style>
<script src="Tests/fixture-vacuus-preact.js"></script>
</head>
<body><div id="mount"/></body>
</rml>)");

	static const TCHAR* GPlainDocument = TEXT(R"(<rml>
<head><style>body { display: block; }</style></head>
<body><div id="plain"/></body>
</rml>)");

	FJsDocProbeHost* Probe = nullptr;
	const uint32 ViewId = AddSeamViewWithDocument(Rig, Probe, TEXT("vacuus_js_remount"), GScriptedDocument);
	if (!TestTrue(TEXT("frames pumped"), PumpRealFrames(*Rig.Thread, 3)))
	{
		return false;
	}

	const FVaCuusJsRuntime* Runtime = FWrappedDomHost::Inner->GetRuntime();
	TestEqual(TEXT("first load: mounted, module ran once"),
		Rig.Eval(ViewId, "[globalThis.RUNS, document.getElementById('probe-btn').innerRML].map(String).join('|')"),
		FString(TEXT("1|n:0")));
	const int64 RefsLoaded = Runtime->GetNumListenerRefs();
	TestTrue(*FString::Printf(TEXT("the mounted app holds listener refs (%lld)"), RefsLoaded), RefsLoaded > 0);

	// Interact, then reload the SAME document — the M2 live-reload shape. The
	// recycle must discard the interaction (state dies with the context) and
	// re-run the module: RUNS is 1 again because globalThis itself is fresh.
	Rig.Eval(ViewId, "document.getElementById('probe-btn').dispatchEvent('click'); 'ok'");
	TestEqual(TEXT("pre-reload interaction committed"),
		Rig.Eval(ViewId, "document.getElementById('probe-btn').innerRML"), FString(TEXT("n:1")));

	Rig.Thread->EnqueueLoadDocumentFromMemory(ViewId, GScriptedDocument, /*LoadSerial=*/2);
	if (!TestTrue(TEXT("reload frames pumped"), PumpRealFrames(*Rig.Thread, 3)))
	{
		return false;
	}
	TestEqual(TEXT("E-P7: reload re-mounts from module top level — fresh context, fresh state"),
		Rig.Eval(ViewId, "[globalThis.RUNS, document.getElementById('probe-btn').innerRML].map(String).join('|')"),
		FString(TEXT("1|n:0")));

	// Replace with a script-less document: the old context dies, nothing
	// re-registers, and the refcount observable answers the leak question.
	Rig.Thread->EnqueueLoadDocumentFromMemory(ViewId, GPlainDocument, /*LoadSerial=*/3);
	if (!TestTrue(TEXT("teardown frames pumped"), PumpRealFrames(*Rig.Thread, 3)))
	{
		return false;
	}
	TestEqual(TEXT("E-P7: listener refs return to zero after the recycle"), Runtime->GetNumListenerRefs(), int64(0));

	TestEqual(TEXT("no JS error anywhere in the run"), Runtime->GetNumErrors(), uint64(0));
	return true;
}

/**
 * vacuus.translate (spec §2(l)) — the snapshot design as shipped: identity +
 * substitution before any table (a params-bearing key works on day one), table
 * lookup + substitution + whole-table replacement with a monotonic version
 * after, and the RML-text half (FVaCuusSystemInterface::TranslateString)
 * reading the SAME snapshot at document parse. Per-call game handlers were
 * REJECTED, not deferred: translate answers synchronously during JS execution
 * on the UI thread, and a game callback there would run game code on the UI
 * thread or turn the API async (FVaCuusTranslationRegistry's header).
 */
bool FVaCuusJsTranslateTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusJsWebDxTest;

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	FDomProbeHost* Probe = nullptr;
	uint32 ViewId = 0;
	if (!BootWithDocument(*this, Rig, Probe, ViewId, TEXT("vacuus_js_translate"), GMountDocument))
	{
		return false;
	}

	// BEFORE ANY TABLE (keys chosen unbindable so suite order cannot matter):
	// identity; substitution over the identity string; bool/number/float/string
	// params; nested objects SKIPPED (the emit contract) so their token stays
	// visibly literal; single braces render literally by RmlUi's own grammar.
	TestEqual(TEXT("no table: identity + params substitution"),
		Rig.Eval(ViewId,
			"[vacuus.translate('vx_untranslated'),"
			" vacuus.translate('vx {a} and {b} and {c} and {d} and {e}',"
			"   {a: 3, b: 3.5, c: true, d: 'str', e: {nested: 1}})]"
			".join('|')"),
		FString(TEXT("vx_untranslated|vx 3 and 3.5 and true and str and {e}")));
	TestEqual(TEXT("non-string key is a TypeError, not a coercion"),
		Rig.Eval(ViewId, "try { vacuus.translate(5); 'no-throw' } catch (e) { 'TypeError:' + (e instanceof TypeError) }"),
		FString(TEXT("TypeError:true")));

	// THE ADVERSARIAL CASE (Task 8 review blocker): a param VALUE carrying
	// another param's token stays literal — killer/victim ARE user data in the
	// killfeed, so a player named 'xX{victim}Xx' must keep that name on screen.
	// The per-param ReplaceInline loop rewrote it ('xXMothXx downed Moth');
	// the single left-to-right pass never rescans appended text. Restore the
	// bug (revert to the loop) and this line reads the rewritten string.
	TestEqual(TEXT("a param value carrying another param's token stays literal"),
		Rig.Eval(ViewId, "vacuus.translate('{killer} downed {victim}', {killer: 'xX{victim}Xx', victim: 'Moth'})"),
		FString(TEXT("xX{victim}Xx downed Moth")));
	TestEqual(TEXT("a self-referential param is trivially safe"),
		Rig.Eval(ViewId, "vacuus.translate('{n}', {n: '{n}'})"), FString(TEXT("{n}")));

	// THE GAME PUSHES A TABLE (the handler seam: SetTranslationTable ->
	// SetTable -> the queue -> the drain's InstallSnapshot). The automation
	// thread IS the game thread and IS the queue's single producer.
	const uint64 VersionBefore = FVaCuusTranslationRegistry::GetVersion_GameThread();
	{
		TMap<FString, FString> Table;
		Table.Add(TEXT("vx_hello"), TEXT("vx_bonjour"));
		Table.Add(TEXT("vx {n} kills"), TEXT("vx {n} frags"));
		FVaCuusTranslationRegistry::SetTable(Table);
	}
	TestEqual(TEXT("the version moved"), FVaCuusTranslationRegistry::GetVersion_GameThread(), VersionBefore + 1);
	if (!TestTrue(TEXT("the snapshot crossed"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}

	TestEqual(TEXT("with a table: lookup, substitution over the translated string, miss = identity"),
		Rig.Eval(ViewId,
			"[vacuus.translate('vx_hello'),"
			" vacuus.translate('vx {n} kills', {n: 7}),"
			" vacuus.translate('vx_absent')].join('|')"),
		FString(TEXT("vx_bonjour|vx 7 frags|vx_absent")));

	// WHOLE-TABLE REPLACEMENT: the second push REPLACES (vx_hello gone), never
	// merges — publish-by-replacement made observable, with the version up.
	{
		TMap<FString, FString> Table;
		Table.Add(TEXT("vx_hello"), TEXT("vx_hallo"));
		FVaCuusTranslationRegistry::SetTable(Table);
	}
	TestEqual(TEXT("the version moved again"), FVaCuusTranslationRegistry::GetVersion_GameThread(), VersionBefore + 2);
	if (!TestTrue(TEXT("the replacement crossed"), PumpRealFrames(*Rig.Thread, 2)))
	{
		return false;
	}
	TestEqual(TEXT("replacement, not merge: the new value answers, the dropped key is identity again"),
		Rig.Eval(ViewId, "[vacuus.translate('vx_hello'), vacuus.translate('vx {n} kills', {n: 7})].join('|')"),
		FString(TEXT("vx_hallo|vx 7 kills")));

	// THE RML HALF: a document loaded AFTER the table renders translated text —
	// RmlUi's own TranslateString path (Factory.cpp:336) through the same
	// snapshot. Loaded on a second view so the first view's document stays put.
	static const TCHAR* GRmlTextDocument = TEXT(R"(<rml>
<head><style>body { display: block; font-family: LatoLatin; font-size: 14px; } div { display: block; }</style></head>
<body><div id="t">vx_hello</div></body>
</rml>)");

	FDomProbeHost* TextProbe = nullptr;
	uint32 TextViewId = 0;
	if (!BootWithDocument(*this, Rig, TextProbe, TextViewId, TEXT("vacuus_js_translate_rml"), GRmlTextDocument))
	{
		return false;
	}
	FString RenderedText;
	Rig.RunOnUI([&RenderedText, TextProbe]()
		{
			Rml::Element* T = TextProbe->GetDocument()->GetElementById("t");
			RenderedText = T != nullptr ? UTF8_TO_TCHAR(T->GetInnerRML().c_str()) : TEXT("<no element>");
		});
	TestEqual(TEXT("RML text translated at parse through the same snapshot"), RenderedText, FString(TEXT("vx_hallo")));

	TestEqual(TEXT("no JS error anywhere in the run"), FWrappedDomHost::Inner->GetRuntime()->GetNumErrors(), uint64(0));
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
