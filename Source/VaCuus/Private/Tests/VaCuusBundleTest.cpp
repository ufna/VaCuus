// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuus.h"
#include "VaCuusBundle.h"
#include "VaCuusBundleMount.h"
#include "VaCuusContentPaths.h"
#include "VaCuusDefines.h"
#include "VaCuusDocumentHost.h"
#include "VaCuusEngine.h"
#include "VaCuusFileInterface.h"
#include "VaCuusTestDocumentHost.h"
#include "VaCuusUIThread.h"
#include "VaCuusViewStatus.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

#include <RmlUi/Core.h>

#include <atomic>
#include <cstdio>

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusBundleTest
{
/**
 * Hand-builds a mountable index + payload WITHOUT the pack: entries laid out on the
 * pack's 64-byte alignment, hash left zero (the mount does not verify it -- the hash
 * is the determinism observable, not an integrity gate). This is what lets the
 * runtime tests corrupt individual fields and watch the exact production refusal.
 */
struct FBundleBuilder
{
	VaCuusBundleFormat::FCookedIndex Index;
	TArray64<uint8> Payload;

	void Add(const TCHAR* Path, const TArray<uint8>& Bytes)
	{
		const int64 Offset = Align(Payload.Num(), VaCuusBundleFormat::EntryAlignment);
		Payload.AddZeroed(Offset - Payload.Num());
		Payload.Append(Bytes.GetData(), Bytes.Num());
		Index.Entries.Add(FVaCuusBundleEntry{VaCuusBundleFormat::NormalizePath(Path), Offset, Bytes.Num()});
		Index.PayloadSize = Payload.Num();
	}

	void AddString(const TCHAR* Path, const ANSICHAR* Content)
	{
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Content), FCStringAnsi::Strlen(Content));
		Add(Path, Bytes);
	}

	bool Mount(const FString& Name)
	{
		VaCuusBundleFormat::FCookedIndex IndexCopy = Index;
		TArray64<uint8> PayloadCopy = Payload;
		return FVaCuusBundleMountTable::MountTransient(Name, TEXT("built by VaCuusBundleTest"),
			MoveTemp(IndexCopy), MoveTemp(PayloadCopy));
	}
};

static Rml::String ToRmlPath(const FString& Path)
{
	return Rml::String(TCHAR_TO_UTF8(*Path));
}

/** Reads a whole open handle through the Rml contract; INDEX_NONE-free helper for the content asserts. */
static TArray<uint8> ReadAll(FVaCuusFileInterface& FileInterface, Rml::FileHandle File)
{
	TArray<uint8> Out;
	Out.SetNumUninitialized(static_cast<int32>(FileInterface.Length(File)));
	FileInterface.Seek(File, 0, SEEK_SET);
	const size_t NumRead = Out.Num() > 0 ? FileInterface.Read(Out.GetData(), Out.Num(), File) : 0;
	Out.SetNum(static_cast<int32>(NumRead));
	return Out;
}
}	 // namespace VaCuusBundleTest

/**
 * The cooked index format: round-trip, then the two refusal classes over
 * HAND-CORRUPTED bytes (spec M6 2(a)'s "corrupted-fixture tests for both") -- a
 * version-mismatch and an implausible entry count, each leaving the archive parked
 * AFTER the block so whatever follows still deserializes (the sentinel proves it).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusBundleFormatTest, "VaCuus.Bundle.Format",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusBundleFormatTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusBundleFormat;

	// --- NormalizePath: the one spelling. ---
	TestEqual(TEXT("Backslashes, case, ./ and duplicate slashes all normalize"),
		NormalizePath(TEXT("./M5Hud\\\\Sub//File.RML")), FString(TEXT("m5hud/sub/file.rml")));
	TestTrue(TEXT("Tests/ at the root is excluded"), IsExcludedTestPath(NormalizePath(TEXT("Tests/probe.js"))));
	TestTrue(TEXT("Tests/ as an inner segment is excluded"), IsExcludedTestPath(NormalizePath(TEXT("img/Tests/x.png"))));
	TestFalse(TEXT("'latest/' is NOT excluded (segment match, not substring)"),
		IsExcludedTestPath(NormalizePath(TEXT("latest/x.rml"))));

	// --- Round-trip. ---
	FCookedIndex Written;
	Written.Entries.Add(FVaCuusBundleEntry{TEXT("a.rml"), 0, 10});
	Written.Entries.Add(FVaCuusBundleEntry{TEXT("b/c.rcss"), 64, 100});
	Written.PayloadSize = 164;
	Written.ContentHash = FBlake3::HashBuffer("determinism", 11);

	TArray<uint8> Bytes;
	constexpr uint32 Sentinel = 0xC0FFEE01;
	{
		FMemoryWriter Writer(Bytes);
		TestTrue(TEXT("Save succeeds"), SerializeCookedIndex(Writer, Written, TEXT("save")));
		uint32 SentinelCopy = Sentinel;
		Writer << SentinelCopy;
	}

	{
		FMemoryReader Reader(Bytes);
		FCookedIndex Read;
		TestTrue(TEXT("Load succeeds"), SerializeCookedIndex(Reader, Read, TEXT("roundtrip")));
		TestEqual(TEXT("Entry count round-trips"), Read.Entries.Num(), 2);
		TestEqual(TEXT("Path round-trips"), Read.Entries[1].Path, FString(TEXT("b/c.rcss")));
		TestEqual(TEXT("Offset round-trips"), Read.Entries[1].Offset, (int64)64);
		TestEqual(TEXT("Payload size round-trips"), Read.PayloadSize, (int64)164);
		TestEqual(TEXT("Hash round-trips"), HashToHex(Read.ContentHash), HashToHex(Written.ContentHash));
		uint32 SentinelRead = 0;
		Reader << SentinelRead;
		TestEqual(TEXT("The stream stays aligned after the block"), SentinelRead, Sentinel);
	}

	// --- Corrupted fixture 1: the version field (first 4 bytes). ---
	{
		TArray<uint8> Corrupted = Bytes;
		Corrupted[0] = 0xFE;
		AddExpectedErrorPlain(TEXT("does not match this build's"), EAutomationExpectedErrorFlags::Contains, 1);

		FMemoryReader Reader(Corrupted);
		FCookedIndex Read;
		TestFalse(TEXT("A version mismatch refuses"), SerializeCookedIndex(Reader, Read, TEXT("bad-version")));
		TestEqual(TEXT("A refused index is empty"), Read.Entries.Num(), 0);

		// The refusal contract that keeps the payload behind the block loadable: the
		// reader must land exactly at the block's end, sentinel-provably.
		uint32 SentinelRead = 0;
		Reader << SentinelRead;
		TestEqual(TEXT("The refusal skipped to the end of the block"), SentinelRead, Sentinel);
	}

	// --- Corrupted fixture 2: the entry count, at a hand-computed offset
	//     ([4 version][8 block bytes][32 hash][8 payload size] -> count at 52). ---
	{
		TArray<uint8> Corrupted = Bytes;
		Corrupted[52] = 0xFF;
		Corrupted[53] = 0xFF;
		Corrupted[54] = 0xFF;
		Corrupted[55] = 0x7F;
		AddExpectedErrorPlain(TEXT("cooked index is corrupt"), EAutomationExpectedErrorFlags::Contains, 1);

		FMemoryReader Reader(Corrupted);
		FCookedIndex Read;
		TestFalse(TEXT("An implausible entry count refuses"), SerializeCookedIndex(Reader, Read, TEXT("bad-count")));
		uint32 SentinelRead = 0;
		Reader << SentinelRead;
		TestEqual(TEXT("That refusal also lands at the block end"), SentinelRead, Sentinel);
	}

	// --- Bounds validation: the mount-time gate, one refusal per violation class.
	//     Each expectation names ITS entry: a logged message is consumed by the first
	//     expectation it matches, so shared fragments ("the bundle is refused") would
	//     miscount across the five refusals below. ---
	{
		AddExpectedErrorPlain(TEXT("('spills.rml') spans"), EAutomationExpectedErrorFlags::Contains, 1);
		TArray<FVaCuusBundleEntry> Bad;
		Bad.Add(FVaCuusBundleEntry{TEXT("spills.rml"), 64, 65});
		TestFalse(TEXT("An entry spilling past the payload refuses"), ValidateEntries(Bad, 128, TEXT("bounds")));
	}
	{
		AddExpectedErrorPlain(TEXT("('negative.rml') spans"), EAutomationExpectedErrorFlags::Contains, 1);
		TArray<FVaCuusBundleEntry> Bad;
		Bad.Add(FVaCuusBundleEntry{TEXT("negative.rml"), -64, 10});
		TestFalse(TEXT("A negative offset refuses"), ValidateEntries(Bad, 128, TEXT("bounds")));
	}
	{
		AddExpectedErrorPlain(TEXT("is not 64-byte aligned"), EAutomationExpectedErrorFlags::Contains, 1);
		TArray<FVaCuusBundleEntry> Bad;
		Bad.Add(FVaCuusBundleEntry{TEXT("askew.rml"), 32, 10});
		TestFalse(TEXT("A misaligned offset refuses"), ValidateEntries(Bad, 128, TEXT("bounds")));
	}
	{
		AddExpectedErrorPlain(TEXT("duplicates the path"), EAutomationExpectedErrorFlags::Contains, 1);
		TArray<FVaCuusBundleEntry> Bad;
		Bad.Add(FVaCuusBundleEntry{TEXT("twice.rml"), 0, 10});
		Bad.Add(FVaCuusBundleEntry{TEXT("twice.rml"), 64, 10});
		TestFalse(TEXT("A duplicate path refuses"), ValidateEntries(Bad, 128, TEXT("bounds")));
	}
	{
		// An overflowing Offset+Size must refuse through the subtraction form, not wrap.
		AddExpectedErrorPlain(TEXT("('overflow.rml') spans"), EAutomationExpectedErrorFlags::Contains, 1);
		TArray<FVaCuusBundleEntry> Bad;
		Bad.Add(FVaCuusBundleEntry{TEXT("overflow.rml"), 64, MAX_int64 - 32});
		TestFalse(TEXT("An overflowing span refuses instead of wrapping"), ValidateEntries(Bad, 128, TEXT("bounds")));
	}

	return true;
}

/**
 * The mount table + the VFS span path: bundle-first over loose, two-bundle overlap
 * first-hit-wins, the miss Warning naming every probed bundle, the span's [0, Size]
 * position contract (exact EOF included -- the FOpenFile guarantee, now trivially
 * held by a span), the serving counter, and ResolveExistingDocument's bundle probe.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusBundleMountTest, "VaCuus.Bundle.Mount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusBundleMountTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusBundleTest;

	ON_SCOPE_EXIT
	{
		// Bundles are process state; a leaked mount would shadow every later test's
		// loose files (bundle-first is the point of the design).
		FVaCuusBundleMountTable::UnmountAll();
	};

	// Bundle-mount validation failures reuse the ValidateEntries/MountTransient
	// refusals already covered above; here everything mounts clean.
	FBundleBuilder BundleA;
	BundleA.AddString(TEXT("only_in_a.rml"), "AAAA");					// 4 bytes
	BundleA.AddString(TEXT("Shared/Doc.rml"), "FROM-A");				// 6 bytes; case folds to shared/doc.rml
	FBundleBuilder BundleB;
	BundleB.AddString(TEXT("shared/doc.rml"), "FROM-B-LONGER");			// 13 bytes
	BundleB.AddString(TEXT("only_in_b.rml"), "BB");						// 2 bytes

	if (!TestTrue(TEXT("Bundle A mounts"), BundleA.Mount(TEXT("<TestBundleA>"))) ||
		!TestTrue(TEXT("Bundle B mounts"), BundleB.Mount(TEXT("<TestBundleB>"))))
	{
		return false;
	}

	FVaCuusFileInterface FileInterface;

	// --- Two-bundle overlap: mounts stack in order, FIRST hit wins (spec M6 2(d)). ---
	{
		const Rml::FileHandle Shared = FileInterface.Open(ToRmlPath(TEXT("shared/doc.rml")));
		if (TestTrue(TEXT("The shared path opens"), Shared != Rml::FileHandle(0)))
		{
			TestEqual(TEXT("The FIRST mounted bundle answers (6 bytes, not 13)"),
				FileInterface.Length(Shared), (size_t)6);
			const TArray<uint8> Content = ReadAll(FileInterface, Shared);
			TestTrue(TEXT("...with bundle A's bytes"),
				Content.Num() == 6 && FMemory::Memcmp(Content.GetData(), "FROM-A", 6) == 0);
			FileInterface.Close(Shared);
		}
	}

	// --- Later bundles still serve what earlier ones lack. ---
	{
		const Rml::FileHandle OnlyB = FileInterface.Open(ToRmlPath(TEXT("only_in_b.rml")));
		if (TestTrue(TEXT("A path only in bundle B opens"), OnlyB != Rml::FileHandle(0)))
		{
			TestEqual(TEXT("...at B's size"), FileInterface.Length(OnlyB), (size_t)2);
			FileInterface.Close(OnlyB);
		}
	}

	// --- The span position contract: exact EOF, refused over-seeks, re-reads. ---
	{
		const Rml::FileHandle File = FileInterface.Open(ToRmlPath(TEXT("only_in_a.rml")));
		if (TestTrue(TEXT("The span handle opens"), File != Rml::FileHandle(0)))
		{
			char Buffer[16] = {0};
			TestTrue(TEXT("Seek to exact EOF succeeds"), FileInterface.Seek(File, 0, SEEK_END));
			TestEqual(TEXT("Tell at exact EOF is Size"), FileInterface.Tell(File), (size_t)4);
			TestEqual(TEXT("Read at exact EOF returns 0"), FileInterface.Read(Buffer, sizeof(Buffer), File), (size_t)0);
			TestFalse(TEXT("Seeking past the end fails"), FileInterface.Seek(File, 1, SEEK_CUR));
			TestTrue(TEXT("SEEK_CUR back from EOF works"), FileInterface.Seek(File, -2, SEEK_CUR));
			TestEqual(TEXT("...and reads the tail"), FileInterface.Read(Buffer, sizeof(Buffer), File), (size_t)2);
			TestTrue(TEXT("...the right tail"), FMemory::Memcmp(Buffer, "AA", 2) == 0);
			FileInterface.Close(File);
		}
	}

	// --- The serving counters: the observability the M==0 gate stands on. ---
	{
		const TSharedPtr<const FVaCuusBundleLookup> Lookup = FVaCuusBundleMountTable::GetLookup();
		if (TestTrue(TEXT("Lookup is published"), Lookup.IsValid()) &&
			TestEqual(TEXT("Two bundles mounted"), Lookup->Mounts.Num(), 2))
		{
			TestEqual(TEXT("Bundle A served the two opens that hit it"),
				(uint64)Lookup->Mounts[0]->ServedOpens.load(), (uint64)2);
			TestEqual(TEXT("Bundle B served the one open that fell through to it"),
				(uint64)Lookup->Mounts[1]->ServedOpens.load(), (uint64)1);
		}
		TestEqual(TEXT("The interface counted 3 bundle opens"), FileInterface.GetNumBundleOpens(), (uint64)3);
		TestEqual(TEXT("...and 0 loose opens"), FileInterface.GetNumLooseOpens(), (uint64)0);
	}

	// --- The miss Warning names every probed bundle (the silent-miss killer). ---
	{
		AddExpectedMessagePlain(TEXT("'vacuus_nowhere.rml' is in NO mounted bundle (probed: <TestBundleA>, <TestBundleB>)"),
			ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
		AddExpectedMessagePlain(TEXT("Failed to open file 'vacuus_nowhere.rml'"),
			ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
		TestEqual(TEXT("A miss still misses (loose fallback found nothing)"),
			FileInterface.Open(ToRmlPath(TEXT("vacuus_nowhere.rml"))), Rml::FileHandle(0));
	}

	// --- ResolveExistingDocument agrees with Open about who serves. ---
	{
		FString Root;
		const FString Resolved = VaCuusContentPaths::ResolveExistingDocument(TEXT("Only_In_A.rml"), &Root);
		TestEqual(TEXT("The resolver reports the bundle pseudo-root"), Root, FString(TEXT("bundle://<TestBundleA>")));
		TestEqual(TEXT("...and the pseudo-path"), Resolved, FString(TEXT("bundle://<TestBundleA>/only_in_a.rml")));
	}

	// --- Bundle-first over a REAL loose file, then the loose fallback after unmount:
	//     precedence proven by shadowing, the ContentRoots test's own method. ---
	{
		const TArray<FString>& Roots = VaCuusContentPaths::GetDocumentRoots();
		if (TestTrue(TEXT("There is a loose root to shadow"), Roots.Num() > 0))
		{
			const FString ProbeName = TEXT("vacuus_bundle_prec_probe.tmptest");
			const FString LoosePath = Roots[0] / ProbeName;
			if (TestTrue(TEXT("Loose probe written"), FFileHelper::SaveStringToFile(TEXT("LOOSECOPY!!"), *LoosePath)))
			{
				FBundleBuilder Shadowing;
				Shadowing.AddString(TEXT("vacuus_bundle_prec_probe.tmptest"), "BUNDLE");	   // 6 vs 11 bytes
				TestTrue(TEXT("The shadowing bundle mounts"), Shadowing.Mount(TEXT("<TestBundleC>")));

				const Rml::FileHandle Shadowed = FileInterface.Open(ToRmlPath(ProbeName));
				if (TestTrue(TEXT("The probe opens with the bundle mounted"), Shadowed != Rml::FileHandle(0)))
				{
					TestEqual(TEXT("THE BUNDLE WINS over the loose copy (6 bytes, not 11)"),
						FileInterface.Length(Shadowed), (size_t)6);
					FileInterface.Close(Shadowed);
				}

				TestTrue(TEXT("Unmount removes the lookup entry"),
					FVaCuusBundleMountTable::UnmountBundle(TEXT("<TestBundleC>")));

				// A and B are still mounted, so the fallback open below is a bundle
				// miss first -- the Warning is the design working, and it is expected
				// rather than silenced.
				AddExpectedMessagePlain(TEXT("'vacuus_bundle_prec_probe.tmptest' is in NO mounted bundle"),
					ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

				const Rml::FileHandle Fallback = FileInterface.Open(ToRmlPath(ProbeName));
				if (TestTrue(TEXT("The probe still opens unmounted"), Fallback != Rml::FileHandle(0)))
				{
					TestEqual(TEXT("...from the LOOSE copy now (11 bytes)"), FileInterface.Length(Fallback), (size_t)11);
					FileInterface.Close(Fallback);
				}
				TestEqual(TEXT("The loose open was counted"), FileInterface.GetNumLooseOpens(), (uint64)1);
			}
			IFileManager::Get().Delete(*LoosePath);
		}
	}

	return true;
}

/**
 * Exp-BUNDLE-UNMOUNT-RACE (spec section 3.1): a reader thread validates a bundle
 * entry's bytes in a loop while the game thread unmounts mid-read. The claim under
 * test is the release rule of spec section 4 -- the record outlives lookup removal,
 * held by the open handle's strong reference, so not one read sees freed memory --
 * and its converse: a NEW open after the unmount misses.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusBundleUnmountRaceTest, "VaCuus.Bundle.UnmountRace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusBundleUnmountRaceTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusBundleTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no second thread to race"));
		return true;
	}

	ON_SCOPE_EXIT
	{
		FVaCuusBundleMountTable::UnmountAll();
	};

	// A 256 KiB entry whose every byte is predictable from its offset, so validation
	// is exact and any read of freed/reused memory is overwhelmingly likely to differ.
	constexpr int32 EntrySize = 256 * 1024;
	TArray<uint8> Pattern;
	Pattern.SetNumUninitialized(EntrySize);
	for (int32 Index = 0; Index < EntrySize; ++Index)
	{
		Pattern[Index] = static_cast<uint8>(Index * 31 + 7);
	}

	FBundleBuilder Builder;
	Builder.Add(TEXT("race_payload.bin"), Pattern);
	if (!TestTrue(TEXT("The race bundle mounts"), Builder.Mount(TEXT("<TestRaceBundle>"))))
	{
		return false;
	}

	FVaCuusFileInterface FileInterface;
	const Rml::FileHandle File = FileInterface.Open(ToRmlPath(TEXT("race_payload.bin")));
	if (!TestTrue(TEXT("The raced handle opens"), File != Rml::FileHandle(0)))
	{
		return false;
	}

	std::atomic<bool> bStop{false};
	std::atomic<int32> Iterations{0};
	std::atomic<int32> Failures{0};

	TFuture<void> Reader = Async(EAsyncExecution::Thread,
		[&FileInterface, File, &bStop, &Iterations, &Failures]()
		{
			uint8 Buffer[4096];
			while (!bStop.load(std::memory_order_relaxed))
			{
				FileInterface.Seek(File, 0, SEEK_SET);
				int64 Total = 0;
				bool bIterationOk = true;
				for (;;)
				{
					const size_t NumRead = FileInterface.Read(Buffer, sizeof(Buffer), File);
					if (NumRead == 0)
					{
						break;
					}
					for (size_t ByteIndex = 0; ByteIndex < NumRead; ++ByteIndex)
					{
						if (Buffer[ByteIndex] != static_cast<uint8>((Total + ByteIndex) * 31 + 7))
						{
							bIterationOk = false;
							break;
						}
					}
					Total += NumRead;
				}
				if (!bIterationOk || Total != EntrySize)
				{
					Failures.fetch_add(1, std::memory_order_relaxed);
				}
				Iterations.fetch_add(1, std::memory_order_relaxed);
			}
		});

	const auto WaitForIterations = [&Iterations](int32 Target) -> bool
	{
		const double Deadline = FPlatformTime::Seconds() + 10.0;
		while (Iterations.load(std::memory_order_relaxed) < Target)
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				return false;
			}
			FPlatformProcess::Sleep(0.001f);
		}
		return true;
	};

	// Let the reader establish a rhythm, unmount UNDER it, then demand a pile of
	// post-unmount iterations before stopping -- those are the reads that would have
	// crashed or mis-read if the unmount freed the region.
	TestTrue(TEXT("The reader got going"), WaitForIterations(5));
	const int32 IterationsBeforeUnmount = Iterations.load(std::memory_order_relaxed);
	TestTrue(TEXT("Unmount succeeded mid-read"), FVaCuusBundleMountTable::UnmountBundle(TEXT("<TestRaceBundle>")));
	TestTrue(TEXT("The reader survived the unmount"), WaitForIterations(IterationsBeforeUnmount + 25));

	bStop.store(true, std::memory_order_relaxed);
	Reader.Wait();

	TestEqual(TEXT("Not one read -- before, across or after the unmount -- saw wrong bytes"),
		Failures.load(std::memory_order_relaxed), 0);
	TestTrue(TEXT("Reads continued after the lookup entry was gone"),
		Iterations.load(std::memory_order_relaxed) >= IterationsBeforeUnmount + 25);

	// The converse: the record only OUTLIVES the lookup, it is not still in it.
	AddExpectedMessagePlain(TEXT("Failed to open file 'race_payload.bin'"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TestEqual(TEXT("A NEW open after the unmount misses"),
		FileInterface.Open(ToRmlPath(TEXT("race_payload.bin"))), Rml::FileHandle(0));

	FileInterface.Close(File);
	return true;
}

#if WITH_EDITOR
/**
 * The pack/read round-trip through the REAL VFS on the REAL UI thread (spec section
 * 3.1), via the same pack-on-demand door PIE parity uses (spec 2(d)): the loose
 * DevUI tree is packed and mounted, RmlUi is booted (its default-font open already
 * goes through the bundle), and a real Rml::Context loads m1_hud.rml BY FILE on the
 * UI thread -- every byte of document, stylesheet and font served from the mounted
 * span, observed through the record's serving counter.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusBundleRoundTripTest, "VaCuus.Bundle.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace VaCuusBundleTest
{
/** The shared probe base reduced to the one thing this test needs: a FILE load. */
class FFileLoadProbeHost final : public FVaCuusTestDocumentHost
{
public:
	FFileLoadProbeHost()
		: FVaCuusTestDocumentHost(TEXT("vacuus_bundle_roundtrip"), "vacuus://bundle_roundtrip.rml", Rml::FocusFlag::Auto)
	{
	}

	/** Through Rml::GetFileInterface(), which is the point: the mounted bundle is what serves it.
	 *  AdoptDocument() supplies the production close/show/report order. */
	virtual void LoadDocumentFromFile(const FString& VfsPath, uint64 LoadSerial) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		AdoptDocument(Context != nullptr ? Context->LoadDocument(Rml::String(TCHAR_TO_UTF8(*VfsPath))) : nullptr, LoadSerial);
	}

	virtual void SetVisible(bool bVisible) override {}

	/**
	 * FALSE ALWAYS, and that is the assertion's shape: everything this test observes -- the
	 * bundle's ServedOpens counter -- happens during the LoadDocument command drain, so the view
	 * must never record a frame. If it did, an Update() would sit between the two ServedOpens
	 * reads and could serve files of its own.
	 */
	virtual bool HasView() const override { return false; }

	virtual void RecordAndPublishFrame() override { checkNoEntry(); }
};
}	 // namespace VaCuusBundleTest

bool FVaCuusBundleRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusBundleTest;

	if (!FPlatformProcess::SupportsMultithreading())
	{
		AddInfo(TEXT("Skipped: no multithreading support, so there is no UI thread to drive"));
		return true;
	}

	if (!TestFalse(TEXT("RmlUi is down before the test"), FVaCuusEngine::Get().IsInitialized()))
	{
		return false;
	}

	// Pack + mount FIRST, boot SECOND: the boot's own default-font load is then the
	// first bundle-served open, exactly as a cooked boot orders it (subsystem
	// Initialize mounts before any view exists).
	if (!TestTrue(TEXT("Pack-on-demand mounts"), FVaCuusBundleMountTable::MountPackedOnDemand()))
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		FVaCuusBundleMountTable::UnmountAll();
	};

	const TSharedPtr<const FVaCuusBundleLookup> Lookup = FVaCuusBundleMountTable::GetLookup();
	if (!TestTrue(TEXT("The lookup holds the on-demand mount"), Lookup.IsValid() && Lookup->Mounts.Num() == 1))
	{
		return false;
	}
	const TSharedRef<FVaCuusBundleMount> Mount = Lookup->Mounts[0];

	TestTrue(TEXT("The real tree packed something"), Mount->Entries.Num() > 0);
	TestTrue(TEXT("The pack carries the shipped HUD document"), Mount->FindEntry(TEXT("m1_hud.rml")) != nullptr);
	TestFalse(TEXT("The pack's content hash is surfaced"), Mount->ContentHashHex.IsEmpty());
	for (const FVaCuusBundleEntry& Entry : Mount->Entries)
	{
		if (!TestFalse(FString::Printf(TEXT("No Tests/ fixture packed ('%s')"), *Entry.Path),
				VaCuusBundleFormat::IsExcludedTestPath(Entry.Path)))
		{
			break;
		}
	}

	FVaCuusModule& Module = FVaCuusModule::Get();
	FVaCuusUIThread* UIThread = Module.GetOrStartUIThread();
	if (!TestNotNull(TEXT("UI thread"), UIThread))
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		Module.StopUIThread();
	};

	const uint64 ServedAfterMount = Mount->ServedOpens.load(std::memory_order_relaxed);

	const TSharedRef<FVaCuusViewStatus> Status = MakeShared<FVaCuusViewStatus>();
	const uint32 ViewId = UIThread->AllocateViewId();
	UIThread->EnqueueAddView(ViewId, MakeUnique<FFileLoadProbeHost>(), FIntPoint(640, 360), Status);
	UIThread->EnqueueLoadDocumentFile(ViewId, TEXT("m1_hud.rml"), /*LoadSerial=*/1);

	UIThread->Trigger();
	const double Deadline = FPlatformTime::Seconds() + 5.0;
	while (Status->LoadCompletedSerial.load(std::memory_order_acquire) < 1 && FPlatformTime::Seconds() < Deadline)
	{
		UIThread->Trigger();
		FPlatformProcess::Sleep(0.005f);
	}

	TestEqual(TEXT("The file load completed on the UI thread"),
		(uint64)Status->LoadCompletedSerial.load(std::memory_order_acquire), (uint64)1);
	TestEqual(TEXT("...and succeeded"), Status->LoadResult.load(std::memory_order_relaxed),
		static_cast<uint8>(EVaCuusLoadResult::Succeeded));

	// The observable: the boot's font open plus the document (and its stylesheet)
	// all landed on the mounted record. Exact counts belong to RmlUi's loader; the
	// claims that are OURS are "the boot served at least one" and "the load served
	// strictly more".
	TestTrue(TEXT("RmlUi's boot (default font) was bundle-served"), ServedAfterMount >= 1);
	TestTrue(TEXT("The document load was bundle-served on the UI thread"),
		Mount->ServedOpens.load(std::memory_order_relaxed) > ServedAfterMount);

	UIThread->EnqueueRemoveView(ViewId);
	UIThread->Trigger();

	return true;
}
#endif	  // WITH_EDITOR

#endif	  // WITH_DEV_AUTOMATION_TESTS
