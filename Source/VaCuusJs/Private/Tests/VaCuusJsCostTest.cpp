// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "VaCuusJsDomTestRig.h"
#include "VaCuusStats.h"

#include "VaCuusContentPaths.h"
#include "VaCuusTestDocumentHost.h"

#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"

/*
 * SPEC 7's BUDGET TABLE, MEASURED (M4 plan Task 10.3) -- the M3a/M3b Cost-harness
 * pattern (VaCuusModelCostTest.cpp) extended to the JS phases: numbers are the
 * deliverable, reported through AddInfo AND UE_LOG so a headless acceptance run can
 * read them from Saved/Logs; assertions are LOOSE tripwires (10x the target, the M3a
 * convention) so a structural regression fails and machine jitter does not.
 *
 * HOW THE PHASES ARE TIMED WITHOUT A SECOND STOPWATCH. RunFrame's own
 * VACUUS_PERF_SCOPE(JsPump)/(JsGC) samplers write an always-on last-sample slot
 * (FVaCuusPerfLog::GetLastSampleMs -- one double per scope, written whether or not the
 * PerfLog cvar is on), and the probe host's RecordAndPublishFrame runs INSIDE the same
 * frame, after the pump: reading the JsPump slot there yields THIS frame's pump cost,
 * and reading the JsGC slot yields the PREVIOUS frame's GC point (which runs after the
 * record loop). So per-frame attribution costs one aligned index shift in
 * post-processing and zero new instrumentation.
 *
 * WHAT "Record" IS IN THIS HARNESS. The probe host, like every M3b cost host, runs
 * Context::Update() and never Context::Render() -- the recording render interface is
 * VaCuusRender's and unreachable from this module. The combined row here is therefore
 * JsPump + Update + JsGC with Record == 0 by construction; the production combined
 * figure, Record included, is read from the headless demo session's PerfLog windows
 * (vacuus.M4Demo + vacuus.M1HUD.PerfLog 1) and recorded beside these numbers in the
 * task report. On the demo-scale documents measured there, Record is a small fraction
 * of Update.
 */
namespace VaCuusJsCostTest
{
using namespace VaCuusJsDomTest;

/**
 * One recorded UI frame of a cost view. Written on the UI thread inside
 * RecordAndPublishFrame; read on the test thread only at indices below a
 * SettledFrames() load (the M3b FArrayFrameRecord hand-off rule, verbatim).
 */
struct FJsCostFrameRecord
{
	/** This frame's Context::Update() bracket, ms. */
	double UpdateMs = 0.0;

	/** This frame's JsPump sample (the pump ran before the record loop). */
	double PumpMs = 0.0;

	/** The PREVIOUS frame's JsGC-point sample (the GC point runs after the record loop). */
	double PrevGcMs = 0.0;

	/** Cumulative collections as of this record; frame i collected iff [i+1] > [i]. */
	uint64 NumCollections = 0;

	/** The runtime's last-collection pause/heap, meaningful at the record after a collection. */
	double LastPauseMs = 0.0;
	uint64 LastHeapBytes = 0;
};

/**
 * FJsDocProbeHost's seam calls (OnDocumentReady after Show, OnDocumentClosing before Close --
 * document scripts must actually run here) with the M3b cost bracket in
 * RecordAndPublishFrame.
 *
 * A SIBLING OF FJsDocProbeHost RATHER THAN A SUBCLASS OF IT: both derive from the shared
 * FVaCuusTestDocumentHost, but this host's RecordAndPublishFrame IS the measurement, and
 * hiding the bracket under a further override seam would put a virtual call inside it. The
 * base is careful to leave RecordAndPublishFrame pure for exactly this reason.
 */
class FJsCostProbeHost final : public FVaCuusTestDocumentHost
{
public:
	explicit FJsCostProbeHost(const TCHAR* InContextPrefix)
		: FVaCuusTestDocumentHost(InContextPrefix, "vacuus://js_cost_test.rml", Rml::FocusFlag::Document)
	{
	}

	/**
	 * RESERVED ONCE, NEVER REALLOCATED (the FArrayCostHost rule): the test thread reads settled
	 * records while a coalesced trigger may still append one more, and Reserve keeps those
	 * earlier-record reads valid. The longest soak here is ~7000 frames; hitting the capacity
	 * would start reallocating under the reader, so the soak length is capped well below it
	 * (MaxSoakFrames).
	 */
	virtual bool OnInitialized() override
	{
		FrameLog.Reserve(16384);
		return true;
	}

	//~ The spec 2(f) seam calls at the production AdoptDocument/CloseDocument placements: this is
	//~ what runs the document's captured scripts, external src resolved through the DevUI roots
	//~ -- the cost documents below lean on both.
	virtual void OnDocumentAdopted() override
	{
		if (IVaCuusScriptHost* ScriptHost = FVaCuusUIThread::GetActiveScriptHost())
		{
			ScriptHost->OnDocumentReady(ViewId, RmlDocument);
		}
	}

	virtual void OnDocumentClosing() override
	{
		if (IVaCuusScriptHost* ScriptHost = FVaCuusUIThread::GetActiveScriptHost())
		{
			ScriptHost->OnDocumentClosing(ViewId);
		}
	}

	virtual void SetVisible(bool /*bVisible*/) override {}

	virtual void RecordAndPublishFrame() override
	{
		check(FVaCuusUIThread::IsInUIThread());

		const double BeforeUpdate = FPlatformTime::Seconds();
		Context->Update();
		const double AfterUpdate = FPlatformTime::Seconds();

		// Outside the bracket, like every M3b cost host's own observations: the perf-slot
		// and counter reads are the instrument, not the cost under measurement.
		FJsCostFrameRecord& Frame = FrameLog.AddDefaulted_GetRef();
		Frame.UpdateMs = (AfterUpdate - BeforeUpdate) * 1000.0;
		Frame.PumpMs = FVaCuusPerfLog::GetLastSampleMs(FVaCuusPerfLog::JsPump);
		Frame.PrevGcMs = FVaCuusPerfLog::GetLastSampleMs(FVaCuusPerfLog::JsGC);
		if (const FVaCuusJsScriptHost* Host = FWrappedDomHost::Inner)
		{
			if (const FVaCuusJsRuntime* Runtime = Host->GetRuntime())
			{
				Frame.NumCollections = Runtime->GetNumCollections();
				Frame.LastPauseMs = Runtime->GetLastCollectionPauseMs();
				Frame.LastHeapBytes = Runtime->GetLastCollectionHeapBytes();
			}
		}

		Status->FramesRecorded.fetch_add(1, std::memory_order_release);
	}

	//~ Post-frame observations; the FArrayFrameRecord hand-off rule.
	TArray<FJsCostFrameRecord> FrameLog;

};

/** FrameLog records the test thread may read; the M3b SettledFrames rule verbatim. */
static int32 SettledFrames(const FVaCuusViewStatus& Status)
{
	return int32(Status.FramesRecorded.load(std::memory_order_acquire));
}

/** Adds a cost-probe view + document, returning the view id and exposing probe + status. */
static uint32 AddCostView(FDomTestRig& Rig, FJsCostProbeHost*& OutProbe, TSharedPtr<FVaCuusViewStatus>& OutStatus,
	const TCHAR* Prefix, const TCHAR* Document)
{
	TUniquePtr<FJsCostProbeHost> Owned = MakeUnique<FJsCostProbeHost>(Prefix);
	OutProbe = Owned.Get();
	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	OutStatus = Status;
	const uint32 ViewId = Rig.Thread->AllocateViewId();
	Rig.Thread->EnqueueAddView(ViewId, MoveTemp(Owned), FIntPoint(400, 800), Status);
	Rig.Thread->EnqueueLoadDocumentFromMemory(ViewId, Document, /*LoadSerial=*/1);
	return ViewId;
}

static bool WaitForLoad(FDomTestRig& Rig, const FVaCuusViewStatus& Status)
{
	for (int32 Attempt = 0; Attempt < 10; ++Attempt)
	{
		if (!PumpRealFrames(*Rig.Thread, 1))
		{
			return false;
		}
		if (Status.LoadCompletedSerial.load(std::memory_order_acquire) == 1)
		{
			return Status.LoadResult.load(std::memory_order_relaxed) == uint8(EVaCuusLoadResult::Succeeded);
		}
	}
	return false;
}

/** Sorted-copy percentile; P in [0,1]. The M3b Describe idiom, kept tiny. */
static double Percentile(TArray<double> Values, double P)
{
	if (Values.IsEmpty())
	{
		return 0.0;
	}
	Values.Sort();
	const int32 Index = FMath::Clamp(int32(P * Values.Num()), 0, Values.Num() - 1);
	return Values[Index];
}

static double Mean(const TArray<double>& Values)
{
	double Sum = 0.0;
	for (const double Value : Values)
	{
		Sum += Value;
	}
	return Values.IsEmpty() ? 0.0 : Sum / Values.Num();
}

/**
 * A JS-BEARING IDLE DOCUMENT: the script ran (context, runtime, one DOM touch) and left
 * NOTHING pending -- no timer, no rAF, no job. What the pump costs on the frames a real
 * HUD spends almost all of its life in.
 */
static const TCHAR* GIdleDocument = TEXT(R"(<rml>
<head><style>body { display: block; font-family: LatoLatin; font-size: 14px; } div { display: block; }</style>
<script>globalThis.idleBooted = (document !== null); document.getElementById('probe').innerRML = 'idle-js-ran';</script>
</head>
<body><div id="probe">idle</div></body>
</rml>)");

/**
 * THE DEMO-PORT STEADY-STATE SHAPE: the REAL m4_hud_logic.js (resolved through the
 * DevUI roots, the same file vacuus.M4Demo runs) over the ids it drives. The
 * data-bound panels are absent -- they contribute to DataApply/Update and are M3a's
 * measured territory; the JsPump row is about what the SCRIPT costs per frame, and
 * that is this document's whole content. With no bound model, get('Health') reads
 * null (one latched Warning, expected below) and the script's documented fallback
 * sweep drives the bar -- same reads, same writes, same per-frame shape as the demo.
 */
static const TCHAR* GSteadyDocument = TEXT(R"(<rml>
<head><style>body { display: block; font-family: LatoLatin; font-size: 14px; } div { display: block; } span { display: inline; }
#js-bar-fill { width: 0%; height: 10px; background-color: #35A7E0; }</style>
<script src="m4_hud_logic.js"></script>
</head>
<body>
	<div id="bar-track"><div id="js-bar-fill"/></div>
	<div class="row"><span id="stance-val"></span></div>
	<div id="p-kills"><span id="kill-count">0</span><div id="kill-rows"></div></div>
	<div id="dmg-zone"></div>
</body>
</rml>)");

/**
 * THE CHURN WORKLOAD (spec 7, plan Task 10.2): the M3b 200-row killfeed fixture shape
 * -- VaCuusKillfeedFixture::MakeRow's `Killer%03d|Victim%03d|Weapon%03d|bool` row
 * text, 200-row population -- driven from JS through the facade at the M3b cost-test
 * rate of one changed row per frame: every rAF appends one fresh-string row and trims
 * one from the front. That is per-frame string/element churn with a standing 200-row
 * population. The trim keeps its own FIFO rather than re-reading `children` (which
 * mints a fresh 200-entry array per read -- allocation the WORKLOAD should not hide
 * the row churn under).
 *
 * WHY EACH ROW ALSO CARRIES A CYCLIC OBJECT GRAPH -- found by this test's own first
 * red run, which measured 0 collections in 6000 frames of pure row churn: quickjs
 * frees acyclic values by reference count the moment the last reference drops, and
 * JS_RunGC exists to free "the GC objects in a cycle" and nothing else
 * (quickjs.c:7078-7089 -> gc_free_cycles, :7027) -- so strings and wrappers dropped
 * by the trim never reach the collector, the live-byte counter stays flat, and the
 * step trigger never arms. A GC POPULATION in this engine means CYCLES, which real
 * component-tree code (the M5 Preact target) produces constantly; each row's node
 * graph below (parent <-> children back-pointers) is that share of the workload, and
 * dropping it on trim is what leaves the collector something only it can free.
 *
 * font-family is load-bearing (the M3b GKillfeedDocument argument): a fontless text
 * element logs per layout pass, and 200 rows of that would bury the log this project
 * reads results from.
 */
static const TCHAR* GChurnDocument = TEXT(R"(<rml>
<head><style>body { display: block; font-family: LatoLatin; font-size: 16px; } div { display: block; }</style>
<script>
'use strict';
var serial = 0;
var fifo = [];
globalThis.churnFrames = 0;
function pad3(n) { return ('00' + (n % 1000)).slice(-3); }
function makeCycle(s)
{
	var node = { serial: s, text: 'row-' + s, kids: [] };
	for (var i = 0; i < 8; i++) { node.kids.push({ parent: node, idx: i, tag: 'kid-' + s + '-' + i }); }
	return node;
}
function tick()
{
	requestAnimationFrame(tick);
	var rows = document.getElementById('rows');
	if (rows === null) { return; }
	var s = serial++;
	var d = document.createElement('div');
	d.innerRML = 'Killer' + pad3(s) + '|Victim' + pad3(s * 7) + '|Weapon' + pad3(s * 13) + '|' + ((s % 2) === 1);
	rows.appendChild(d);
	fifo.push({ el: d, data: makeCycle(s) });
	while (fifo.length > 200) { fifo.shift().el.remove(); }
	globalThis.churnFrames++;
}
requestAnimationFrame(tick);
</script>
</head>
<body><div id="rows"></div></body>
</rml>)");

/** A small facade-ops document: one probe element, one trivial script to materialize the context. */
static const TCHAR* GFacadeOpsDocument = TEXT(R"(<rml>
<head><style>body { display: block; font-family: LatoLatin; font-size: 14px; } div { display: block; }</style>
<script>globalThis.ready = 1;</script>
</head>
<body><div id="probe">x</div></body>
</rml>)");
}	 // namespace VaCuusJsCostTest

/**
 * SPEC 7 ROW 1 + THE IDLE ROW's JS COUNTERS: JsPump with nothing due (target 0.02 ms)
 * and the exact-zero fired-counters over a settled window -- 0 timers fired, 0 rAF
 * run, 0 jobs executed, 0 collections (spec 6's counters; the M3b idle gates' model
 * halves -- 0 published / 0 applied / 0 evaluated with a bound model under a scripted
 * document -- are VaCuus.Js.Cost.IdleExactZeros, in the VaCuus module where the model
 * machinery lives).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsCostPumpIdleTest, "VaCuus.Js.Cost.PumpIdle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusJsCostPumpIdleTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsCostTest;

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	FJsCostProbeHost* Probe = nullptr;
	TSharedPtr<FVaCuusViewStatus> Status;
	const uint32 ViewId = AddCostView(Rig, Probe, Status, TEXT("vacuus_jscost_idle"), GIdleDocument);
	if (!TestTrue(TEXT("the idle document loaded"), WaitForLoad(Rig, *Status)))
	{
		return false;
	}
	if (!TestEqual(TEXT("the idle script ran"), Rig.Eval(ViewId, "String(globalThis.idleBooted)"), FString(TEXT("true"))))
	{
		return false;
	}

	// Warm-up: the eval above ran through a RunOnUI closure; give the pump a few
	// closure-free frames before anything is measured.
	if (!TestTrue(TEXT("warm-up frames ran"), PumpRealFrames(*Rig.Thread, 40)))
	{
		return false;
	}

	const FVaCuusJsRuntime* Runtime = FWrappedDomHost::Inner ? FWrappedDomHost::Inner->GetRuntime() : nullptr;
	if (!TestNotNull(TEXT("the runtime exists (the idle script created it)"), Runtime))
	{
		return false;
	}

	const uint64 TimersBefore = Runtime->GetNumTimersFired();
	const uint64 RafBefore = Runtime->GetNumRafCallbacksRun();
	const uint64 JobsBefore = Runtime->GetNumJobsExecuted();
	const uint64 CollectionsBefore = Runtime->GetNumCollections();
	const uint64 ErrorsBefore = Runtime->GetNumErrors();
	const int32 WindowStart = SettledFrames(*Status);

	if (!TestTrue(TEXT("the idle window ran"), PumpRealFrames(*Rig.Thread, 400)))
	{
		return false;
	}

	const int32 WindowEnd = SettledFrames(*Status);

	// ---- The idle row's exact zeros (spec 7: "ALL exact counters"). ----
	TestEqual(TEXT("0 timers fired across the idle window"), Runtime->GetNumTimersFired(), TimersBefore);
	TestEqual(TEXT("0 rAF callbacks run"), Runtime->GetNumRafCallbacksRun(), RafBefore);
	TestEqual(TEXT("0 jobs executed"), Runtime->GetNumJobsExecuted(), JobsBefore);
	TestEqual(TEXT("0 collections (nothing allocated, so the step trigger never armed)"),
		Runtime->GetNumCollections(), CollectionsBefore);
	TestEqual(TEXT("0 JS errors"), Runtime->GetNumErrors(), ErrorsBefore);

	// ---- Row 1: JsPump, idle. ----
	TArray<double> PumpMs;
	for (int32 Index = WindowStart; Index < WindowEnd; ++Index)
	{
		PumpMs.Add(Probe->FrameLog[Index].PumpMs);
	}

	const double PumpMean = Mean(PumpMs);
	const double PumpP99 = Percentile(PumpMs, 0.99);
	const FString Report = FString::Printf(
		TEXT("JsPump idle (nothing due, %d frames): mean %.5f ms, p99 %.5f ms; target 0.02 ms"),
		PumpMs.Num(), PumpMean, PumpP99);
	AddInfo(Report);
	UE_LOG(LogVaCuusJS, Display, TEXT("VaCuus M4 cost: %s"), *Report);

	TestTrue(*FString::Printf(TEXT("idle JsPump stays inside 10x the budget (%.5f ms)"), PumpMean), PumpMean < 0.2);

	Rig.Thread->EnqueueRemoveView(ViewId);
	PumpRealFrames(*Rig.Thread, 1);
	return true;
}

/**
 * SPEC 7 ROW 2: JsPump steady state on the demo-port document (target 0.30 ms) --
 * the real m4_hud_logic.js: a rAF callback per frame (model read + change-gated style
 * write + stance beat), the 1.5 s / 0.9 s intervals amortized across the window.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsCostPumpSteadyTest, "VaCuus.Js.Cost.PumpSteadyDemo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusJsCostPumpSteadyTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsCostTest;

	// The script's documented model-read miss: no game feeds `hud` in this harness, the
	// read latches ONE Warning and the fallback sweep takes over.
	AddExpectedMessagePlain(TEXT("model 'hud', path 'Health'"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	FJsCostProbeHost* Probe = nullptr;
	TSharedPtr<FVaCuusViewStatus> Status;
	const uint32 ViewId = AddCostView(Rig, Probe, Status, TEXT("vacuus_jscost_steady"), GSteadyDocument);
	if (!TestTrue(TEXT("the steady document loaded"), WaitForLoad(Rig, *Status)))
	{
		return false;
	}

	// The demo script must actually be driving: its rAF re-arms every frame.
	const FVaCuusJsRuntime* Runtime = FWrappedDomHost::Inner ? FWrappedDomHost::Inner->GetRuntime() : nullptr;
	if (!TestNotNull(TEXT("the runtime exists"), Runtime))
	{
		return false;
	}

	if (!TestTrue(TEXT("warm-up frames ran"), PumpRealFrames(*Rig.Thread, 60)))
	{
		return false;
	}

	const uint64 RafBefore = Runtime->GetNumRafCallbacksRun();
	const uint64 ErrorsBefore = Runtime->GetNumErrors();
	const int32 WindowStart = SettledFrames(*Status);

	if (!TestTrue(TEXT("the steady window ran"), PumpRealFrames(*Rig.Thread, 2000)))
	{
		return false;
	}

	const int32 WindowEnd = SettledFrames(*Status);
	const uint64 RafRun = Runtime->GetNumRafCallbacksRun() - RafBefore;

	TestTrue(*FString::Printf(TEXT("the demo rAF ran once per frame (%llu callbacks over %d frames)"), RafRun,
				 WindowEnd - WindowStart),
		RafRun >= uint64(WindowEnd - WindowStart));
	TestEqual(TEXT("0 JS errors across the steady window"), Runtime->GetNumErrors(), ErrorsBefore);

	TArray<double> PumpMs;
	for (int32 Index = WindowStart; Index < WindowEnd; ++Index)
	{
		PumpMs.Add(Probe->FrameLog[Index].PumpMs);
	}

	const double PumpMean = Mean(PumpMs);
	const double PumpP99 = Percentile(PumpMs, 0.99);
	const FString Report = FString::Printf(
		TEXT("JsPump steady state, demo-port document (m4_hud_logic.js, %d frames): mean %.5f ms, p99 %.5f ms; ")
		TEXT("target 0.30 ms"),
		PumpMs.Num(), PumpMean, PumpP99);
	AddInfo(Report);
	UE_LOG(LogVaCuusJS, Display, TEXT("VaCuus M4 cost: %s"), *Report);

	TestTrue(*FString::Printf(TEXT("steady JsPump stays inside 10x the budget (%.5f ms)"), PumpMean), PumpMean < 3.0);

	Rig.Thread->EnqueueRemoveView(ViewId);
	PumpRealFrames(*Rig.Thread, 1);
	return true;
}

/**
 * THE M5 RE-MEASUREMENT (M5 spec §6, "Preact HUD steady state — the M4 row
 * re-measured on the port"): the SAME method and budget as PumpSteadyDemo one
 * test up, on the committed TSX HUD bundle (@vacuus/preact over the facade)
 * loaded through the real captured <script src> path. The HUD's steady shape
 * mirrors the M4 script's deliberately — one rAF per frame reading
 * model('hud').Health with a change-gated setState commit, a 1.5 s killfeed
 * beat — so the two rows compare like for like. Skips (green, with the reason)
 * when the committed bundle's provenance is stale against the current facade
 * manifest: the measurement would then be of a bundle the suite already told
 * the controller to rebuild (VaCuus.Js.Preact.BundleMount owns that warning).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsCostPumpSteadyTsxTest, "VaCuus.Js.Cost.PumpSteadyTsx",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusJsCostPumpSteadyTsxTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsCostTest;

	// The provenance gate, quiet flavor (the named warning is BundleMount's).
	{
		const TArray<FString>& Roots = VaCuusContentPaths::GetDocumentRoots();
		FString Provenance;
		TArray<uint8> ManifestBytes;
		if (Roots.IsEmpty() ||
			!FFileHelper::LoadFileToString(Provenance, *(Roots[0] / TEXT("M5Hud/hud_bundle.provenance.json"))) ||
			!FFileHelper::LoadFileToArray(ManifestBytes, *(Roots[0] / TEXT("Tests/vacuus-api-manifest.json"))))
		{
			AddError(TEXT("the committed M5Hud provenance or the facade manifest is missing"));
			return false;
		}
		FSHAHash Hash;
		FSHA1::HashBuffer(ManifestBytes.GetData(), ManifestBytes.Num(), Hash.Hash);
		if (!Provenance.Contains(Hash.ToString().ToLower()))
		{
			AddInfo(TEXT("skipped: hud_bundle.js provenance is stale against the facade manifest — rebuild via ")
				TEXT("`node Web/packages/cli/bin/vacuus.mjs build --app Web/apps/demo-hud` (BundleMount carries the warning)"));
			return true;
		}
	}

	// The TSX HUD reads 'hud'.Health per rAF frame; unbound here, so the read
	// surface latches ONE Warning and the deterministic fallback sweep drives
	// the bar (the demo-port test's own shape, one seat up).
	AddExpectedMessagePlain(TEXT("model 'hud', path 'Health'"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	static const TCHAR* GTsxDocument = TEXT(R"(<rml>
<head><style>body { display: block; font-family: LatoLatin; font-size: 14px; } div { display: block; } button { display: inline-block; } h1 { display: block; } span { display: inline; }</style>
<script src="M5Hud/hud_bundle.js"></script>
</head>
<body><div id="mount"/></body>
</rml>)");

	FJsCostProbeHost* Probe = nullptr;
	TSharedPtr<FVaCuusViewStatus> Status;
	const uint32 ViewId = AddCostView(Rig, Probe, Status, TEXT("vacuus_jscost_tsx"), GTsxDocument);
	if (!TestTrue(TEXT("the TSX document loaded"), WaitForLoad(Rig, *Status)))
	{
		return false;
	}

	const FVaCuusJsRuntime* Runtime = FWrappedDomHost::Inner ? FWrappedDomHost::Inner->GetRuntime() : nullptr;
	if (!TestNotNull(TEXT("the runtime exists"), Runtime))
	{
		return false;
	}

	// The mount must be real before anything is measured — a bundle that failed
	// to mount would "measure" an idle pump and pass on nothing.
	if (!TestEqual(TEXT("the TSX HUD mounted"),
			Rig.Eval(ViewId, "document.getElementById('hud-root') !== null ? 'yes' : 'no'"), FString(TEXT("yes"))))
	{
		return false;
	}

	if (!TestTrue(TEXT("warm-up frames ran"), PumpRealFrames(*Rig.Thread, 60)))
	{
		return false;
	}

	const uint64 RafBefore = Runtime->GetNumRafCallbacksRun();
	const uint64 ErrorsBefore = Runtime->GetNumErrors();
	const int32 WindowStart = SettledFrames(*Status);

	if (!TestTrue(TEXT("the steady window ran"), PumpRealFrames(*Rig.Thread, 2000)))
	{
		return false;
	}

	const int32 WindowEnd = SettledFrames(*Status);
	const uint64 RafRun = Runtime->GetNumRafCallbacksRun() - RafBefore;

	TestTrue(*FString::Printf(TEXT("the HUD rAF ran once per frame (%llu callbacks over %d frames)"), RafRun,
				 WindowEnd - WindowStart),
		RafRun >= uint64(WindowEnd - WindowStart));
	TestEqual(TEXT("0 JS errors across the steady window"), Runtime->GetNumErrors(), ErrorsBefore);

	TArray<double> PumpMs;
	for (int32 Index = WindowStart; Index < WindowEnd; ++Index)
	{
		PumpMs.Add(Probe->FrameLog[Index].PumpMs);
	}

	const double PumpMean = Mean(PumpMs);
	const double PumpP99 = Percentile(PumpMs, 0.99);
	const FString Report = FString::Printf(
		TEXT("JsPump steady state, TSX HUD (@vacuus/preact hud_bundle.js, %d frames): mean %.5f ms, p99 %.5f ms; ")
		TEXT("target 0.30 ms (M5 spec §6, the M4 row re-measured)"),
		PumpMs.Num(), PumpMean, PumpP99);
	AddInfo(Report);
	UE_LOG(LogVaCuusJS, Display, TEXT("VaCuus M5 cost: %s"), *Report);

	// The M4 convention: the tripwire is 10x the budget so a structural
	// regression fails and machine jitter does not.
	TestTrue(*FString::Printf(TEXT("steady TSX JsPump stays inside 10x the budget (%.5f ms)"), PumpMean), PumpMean < 3.0);

	Rig.Thread->EnqueueRemoveView(ViewId);
	PumpRealFrames(*Rig.Thread, 1);
	return true;
}

/**
 * THE GATE ROW (spec 7): p99 of the per-frame sum JsPump + JsGC + Update (+ Record,
 * which is 0 in this harness -- the file comment) over a churn soak with collections
 * in the population, GC step at its default. Plus the GC rows: pause p50/p99 per
 * collection, collections per window, heap at collection.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsCostCombinedChurnTest, "VaCuus.Js.Cost.CombinedChurn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusJsCostCombinedChurnTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsCostTest;

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	FJsCostProbeHost* Probe = nullptr;
	TSharedPtr<FVaCuusViewStatus> Status;
	const uint32 ViewId = AddCostView(Rig, Probe, Status, TEXT("vacuus_jscost_churn"), GChurnDocument);
	if (!TestTrue(TEXT("the churn document loaded"), WaitForLoad(Rig, *Status)))
	{
		return false;
	}

	const FVaCuusJsRuntime* Runtime = FWrappedDomHost::Inner ? FWrappedDomHost::Inner->GetRuntime() : nullptr;
	if (!TestNotNull(TEXT("the runtime exists"), Runtime))
	{
		return false;
	}
	const uint64 ErrorsBefore = Runtime->GetNumErrors();

	// Population build-up: 200 frames to grow the 200-row feed, then settle.
	if (!TestTrue(TEXT("the population built"), PumpRealFrames(*Rig.Thread, 240)))
	{
		return false;
	}

	// The soak: at least 2000 frames, extended in blocks until the GC population is
	// real (>= 3 collections) or the cap is reached -- the collection RATE at the
	// default step is itself a measured number, not something to force.
	constexpr int32 MaxSoakFrames = 6000;
	const int32 WindowStart = SettledFrames(*Status);
	const uint64 CollectionsBefore = Runtime->GetNumCollections();

	int32 SoakFrames = 0;
	while (SoakFrames < 2000 ||
		   (Runtime->GetNumCollections() - CollectionsBefore < 3 && SoakFrames < MaxSoakFrames))
	{
		if (!TestTrue(TEXT("a soak block ran"), PumpRealFrames(*Rig.Thread, 500)))
		{
			return false;
		}
		SoakFrames += 500;
	}

	const int32 WindowEnd = SettledFrames(*Status);
	TestEqual(TEXT("0 JS errors across the soak"), Runtime->GetNumErrors(), ErrorsBefore);

	// The churn really ran every frame: its own counter is the proof the workload was
	// present in every measured frame rather than dying quietly at frame 3.
	const FString ChurnFrames = Rig.Eval(ViewId, "String(globalThis.churnFrames)");
	const int32 NumChurnFrames = FCString::Atoi(*ChurnFrames);
	TestTrue(*FString::Printf(TEXT("the churn rAF ran every frame (%d ticks, window %d frames)"), NumChurnFrames,
				 WindowEnd - WindowStart),
		NumChurnFrames >= WindowEnd - WindowStart);

	// ---- Post-processing: the aligned per-frame sums (the file comment's index shift:
	// frame i's GC point is record i+1's PrevGcMs; a collection in frame i is a
	// cumulative-count step between records i and i+1). ----
	TArray<double> CombinedMs, PumpMs, UpdateMs, GcMs, PausesMs;
	uint64 MaxHeapAtCollection = 0;
	for (int32 Index = WindowStart; Index < WindowEnd - 1; ++Index)
	{
		const FJsCostFrameRecord& Frame = Probe->FrameLog[Index];
		const FJsCostFrameRecord& Next = Probe->FrameLog[Index + 1];
		const double FrameGcMs = Next.PrevGcMs;
		PumpMs.Add(Frame.PumpMs);
		UpdateMs.Add(Frame.UpdateMs);
		GcMs.Add(FrameGcMs);
		CombinedMs.Add(Frame.PumpMs + Frame.UpdateMs + FrameGcMs);
		if (Next.NumCollections > Frame.NumCollections)
		{
			PausesMs.Add(Next.LastPauseMs);
			MaxHeapAtCollection = FMath::Max(MaxHeapAtCollection, Next.LastHeapBytes);
		}
	}

	const double CombinedP99 = Percentile(CombinedMs, 0.99);
	const double CombinedP50 = Percentile(CombinedMs, 0.5);
	const double CombinedMax = Percentile(CombinedMs, 1.0);
	const int32 NumCollections = PausesMs.Num();
	const double WindowSeconds = double(CombinedMs.Num()) *
		(FVaCuusPerfLog::GetLastUIFrameIntervalSeconds() > 0.0 ? FVaCuusPerfLog::GetLastUIFrameIntervalSeconds() : 0.0);

	const FString CombinedReport = FString::Printf(
		TEXT("COMBINED per-frame sum (JsPump + Update + JsGC; Record==0 in this harness) over %d churn frames: ")
		TEXT("p50 %.5f, p99 %.5f, max %.5f ms, tail ratio %.2f; BUDGET p99 <= 0.50 ms (the spec 7 target -- what is ")
		TEXT("ASSERTED is p50 < 3.0 and p99 < 10x p50, see the tripwire comment) | parts mean: pump %.5f, update %.5f, ")
		TEXT("gc-point %.5f ms"),
		CombinedMs.Num(), CombinedP50, CombinedP99, CombinedMax, CombinedP50 > 0.0 ? CombinedP99 / CombinedP50 : 0.0,
		Mean(PumpMs), Mean(UpdateMs), Mean(GcMs));
	AddInfo(CombinedReport);
	UE_LOG(LogVaCuusJS, Display, TEXT("VaCuus M4 cost: %s"), *CombinedReport);

	const FString GcReport = FString::Printf(
		TEXT("JsGC on the churn workload (step at default): %d collections in %d frames (~%.2f s); pause p50 %.5f, ")
		TEXT("p99 %.5f, max %.5f ms; heap at collection max %.1f KB (cap 16 MB)"),
		NumCollections, CombinedMs.Num(), WindowSeconds, Percentile(PausesMs, 0.5), Percentile(PausesMs, 0.99),
		Percentile(PausesMs, 1.0), double(MaxHeapAtCollection) / 1024.0);
	AddInfo(GcReport);
	UE_LOG(LogVaCuusJS, Display, TEXT("VaCuus M4 cost: %s"), *GcReport);

	// THE TRIPWIRES, AND WHAT CHANGED (bead VaCuus-akj.27, 2026-08-07). The p99 used to be
	// gated directly at 10x the 0.50 ms budget, on the stated theory that "a structural
	// regression fails and machine jitter does not". Measured, that theory does not hold across
	// machines -- three consecutive runs each, same commit, same binary:
	//
	//   Linux, 16-core dev box     p50 0.389-0.392   p99 0.635-0.689   ratio 1.6-1.8
	//   macOS, M1 Pro desktop      p50 1.40 -1.48    p99 4.62 -5.39    ratio 3.3-3.6
	//
	// The Mac is 3.6x slower at the MEDIAN and carries twice the relative tail, so it straddles
	// the 5.0 ms line and the outcome is a coin flip -- it passed and failed within the same
	// hour. An absolute millisecond bound cannot express "no structural regression" across
	// machines that differ 3.6x on the steady frame, and a gate that fails half the time on a
	// supported platform gets muted, which costs more than it protects.
	//
	// So the two axes are gated separately, each by the quantity that is actually stable:
	//
	//  - THE MEDIAN, absolutely. It is the per-frame cost of our code and it is the quantity a
	//    structural regression moves. It is also remarkably steady -- +/-0.5% on Linux, +/-3% on
	//    the Mac. 3.0 ms is this file's own 10x-the-budget idiom, already used by the steady-TSX
	//    row above (PumpMean < 3.0); it leaves Linux 7.7x of headroom and the Mac 2.1x.
	//
	//  - THE TAIL, REPORTED AND NOT ASSERTED. A relative bound (p99/p50) was tried first and is
	//    ALSO not robust, which is worth recording because it is the obvious next idea: the same
	//    Mac measured ratio 3.3-3.6 quiet and 21.35 once a 350 MB checkout on the same disk gave
	//    Spotlight something to index -- one frame in that run stalled for 417 ms. No ratio
	//    survives a desktop OS deciding to do IO, because the outlier is unbounded. So the tail
	//    is printed, loudly when it is out of family, and never fails the suite.
	//
	// THE FULL EVIDENCE, every sample taken 2026-08-07/08, so the next person re-calibrating has
	// the spread and not just a number:
	//     p50   Linux quiet 0.389 0.388 0.391 0.389 | Mac quiet 1.40 1.45 1.48 | Mac busy 1.40 1.54
	//     p99   Linux quiet 0.635 0.689 0.635 0.639 | Mac quiet 4.62 4.95 5.39 | Mac busy 12.6 32.8
	// p50 moves 0.8% on Linux and 6% on the Mac ACROSS BOTH MACHINE STATES; p99 moves 5x on one
	// machine. That is the whole argument for which one is the gate.
	//
	// THE P99 IS STILL THE DELIVERABLE -- spec 7 asks for it and it is still reported above. What
	// this section decides is only what may turn the suite red.
	TestTrue(*FString::Printf(TEXT("the combined p50 stays inside 10x the gate (%.5f ms)"), CombinedP50),
		CombinedP50 < 3.0);

	// Out-of-family tails are worth a line even though they are not a failure: on a quiet machine
	// this never prints, so when it does, either the machine was busy or something really did grow
	// a tail -- and the max frame next to it usually says which.
	const double TailRatio = CombinedP50 > 0.0 ? CombinedP99 / CombinedP50 : 0.0;
	if (TailRatio > 10.0)
	{
		const FString TailNote = FString::Printf(
			TEXT("tail out of family: p99 %.5f ms is %.1fx p50 %.5f ms (max frame %.2f ms). NOT a failure -- see the ")
			TEXT("tripwire comment. On a quiet machine this line does not appear; if it does on CI, check what else ")
			TEXT("was running before reading it as a regression."),
			CombinedP99, TailRatio, CombinedP50, CombinedMax);
		AddInfo(TailNote);
		UE_LOG(LogVaCuusJS, Warning, TEXT("VaCuus M4 cost: %s"), *TailNote);
	}
	TestTrue(*FString::Printf(TEXT("collections were in the soak population (%d)"), NumCollections), NumCollections >= 1);
	TestTrue(*FString::Printf(TEXT("heap at collection stays under the 16 MB cap (%.1f KB)"),
				 double(MaxHeapAtCollection) / 1024.0),
		MaxHeapAtCollection < 16ull * 1024 * 1024);

	Rig.Thread->EnqueueRemoveView(ViewId);
	PumpRealFrames(*Rig.Thread, 1);
	return true;
}

/**
 * SPEC 7's FACADE-OP ROW: wrap (createElement -- element construction + fresh
 * wrapper), getElementById (tree lookup + identity-cache hit) and style setProperty,
 * as microseconds per op from an in-JS loop with an empty-loop baseline subtracted.
 * Measure, no target -- the docs-table row.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusJsCostFacadeOpsTest, "VaCuus.Js.Cost.FacadeOps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusJsCostFacadeOpsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace VaCuusJsCostTest;

	// A generous direct watchdog (not the cvar): 10k facade ops under one entry guard
	// is exactly the loop a loaded CI machine could push past the 250 ms default, and a
	// trip here would measure the watchdog, not the facade.
	FVaCuusJsScriptHost::FParams Params;
	Params.RuntimeParams.WatchdogMs = 10000;

	FDomTestRig Rig;
	const FDomTestRig::EBoot Boot = Rig.Boot(*this, Params);
	if (Boot != FDomTestRig::EBoot::Ok)
	{
		return Boot == FDomTestRig::EBoot::Skip;
	}

	FJsCostProbeHost* Probe = nullptr;
	TSharedPtr<FVaCuusViewStatus> Status;
	const uint32 ViewId = AddCostView(Rig, Probe, Status, TEXT("vacuus_jscost_facade"), GFacadeOpsDocument);
	if (!TestTrue(TEXT("the facade-ops document loaded"), WaitForLoad(Rig, *Status)))
	{
		return false;
	}

	constexpr int32 NumOps = 10000;
	double BaselineMs = 0.0, GetByIdMs = 0.0, CreateMs = 0.0, StyleMs = 0.0;
	const bool bRan = Rig.RunOnUI(
		[&]()
		{
			FVaCuusJsScriptHost& Host = *FWrappedDomHost::Inner;
			auto TimeEval = [&Host, ViewId](const char* Source)
			{
				const double Before = FPlatformTime::Seconds();
				EvalString(Host, ViewId, Source);
				return (FPlatformTime::Seconds() - Before) * 1000.0;
			};

			BaselineMs = TimeEval("for (var i = 0; i < 10000; i++) {}");
			GetByIdMs = TimeEval("for (var i = 0; i < 10000; i++) { document.getElementById('probe'); }");
			CreateMs = TimeEval("for (var i = 0; i < 10000; i++) { document.createElement('div'); }");
			StyleMs = TimeEval(
				"(function(){ var e = document.getElementById('probe');"
				" for (var i = 0; i < 10000; i++) { e.style.width = ((i & 1) ? '10px' : '11px'); } })()");
		});
	if (!TestTrue(TEXT("the op loops ran on the UI thread"), bRan))
	{
		return false;
	}

	const FVaCuusJsRuntime* Runtime = FWrappedDomHost::Inner ? FWrappedDomHost::Inner->GetRuntime() : nullptr;
	if (TestNotNull(TEXT("the runtime exists"), Runtime))
	{
		TestEqual(TEXT("0 JS errors (no watchdog trip inside the loops)"), Runtime->GetNumErrors(), 0ull);
	}

	auto PerOpUs = [&](double LoopMs) { return FMath::Max(0.0, LoopMs - BaselineMs) * 1000.0 / NumOps; };
	const FString Report = FString::Printf(
		TEXT("facade ops, us/op over %d ops (empty-loop baseline %.3f ms subtracted): getElementById %.3f | ")
		TEXT("createElement+wrap %.3f (includes Rml element construction; the 10k detached elements are freed by the ")
		TEXT("wrappers' finalizers at the following collections) | style set %.3f; measure-only row, no target"),
		NumOps, BaselineMs, PerOpUs(GetByIdMs), PerOpUs(CreateMs), PerOpUs(StyleMs));
	AddInfo(Report);
	UE_LOG(LogVaCuusJS, Display, TEXT("VaCuus M4 cost: %s"), *Report);

	Rig.Thread->EnqueueRemoveView(ViewId);
	PumpRealFrames(*Rig.Thread, 1);
	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
