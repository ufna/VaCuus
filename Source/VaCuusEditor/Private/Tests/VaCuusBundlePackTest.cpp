// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "VaCuusBundle.h"

#include "Algo/Reverse.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace VaCuusBundlePackTest
{
/**
 * Captures GLog lines across a scope, because AddExpectedMessage CANNOT see them:
 * the automation output device only routes Error/Warning/Display into the matching
 * machinery (AutomationTest.cpp:233 -- the verbosity filter enumerates exactly those
 * three), so an expectation at ELogVerbosity::Log is unsatisfiable by construction.
 * The shadow line under test is Log ON PURPOSE -- a per-file pack diagnostic is not
 * a Display-level product line -- so the test reads the stream itself.
 */
class FScopedLogCapture final : public FOutputDevice
{
public:
	FScopedLogCapture()
	{
		GLog->AddOutputDevice(this);
	}

	virtual ~FScopedLogCapture() override
	{
		GLog->RemoveOutputDevice(this);
	}

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		FScopeLock Lock(&Mutex);
		Lines.Add(V);
	}

	bool Contains(const TCHAR* Fragment)
	{
		// Threaded logging delivers to registered devices from the log thread; the
		// flush is what makes "emitted before this line" checkable.
		GLog->FlushThreadedLogs();
		FScopeLock Lock(&Mutex);
		for (const FString& Line : Lines)
		{
			if (Line.Contains(Fragment))
			{
				return true;
			}
		}
		return false;
	}

private:
	FCriticalSection Mutex;
	TArray<FString> Lines;
};

struct FFixtureTree
{
	FString Root;

	explicit FFixtureTree(const TCHAR* Name)
	{
		Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaCuusBundlePackTest") / Name);
		IFileManager::Get().DeleteDirectory(*Root, /*RequireExists*/ false, /*Tree*/ true);
	}

	~FFixtureTree()
	{
		IFileManager::Get().DeleteDirectory(*Root, /*RequireExists*/ false, /*Tree*/ true);
	}

	bool Write(const TCHAR* RelativePath, const TCHAR* Content) const
	{
		return FFileHelper::SaveStringToFile(Content, *(Root / RelativePath));
	}
};
}	 // namespace VaCuusBundlePackTest

/**
 * The determinism double-pack (spec M6 2(c)): the identical tree packed from
 * differently-ordered inputs must produce BYTE-IDENTICAL payload and index hash --
 * the property incremental/multi-process cooks compare result hashes over, given its
 * observable. Plus the negative control: one changed byte moves the hash.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusBundlePackDeterminismTest, "VaCuus.Bundle.PackDeterminism",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusBundlePackDeterminismTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusBundlePackTest;

	FFixtureTree Tree(TEXT("determinism"));
	if (!TestTrue(TEXT("Fixture written"),
			Tree.Write(TEXT("b.rml"), TEXT("<rml>B</rml>")) &&
			Tree.Write(TEXT("a.rcss"), TEXT("body { }")) &&
			Tree.Write(TEXT("sub/c.js"), TEXT("let c = 3;"))))
	{
		return false;
	}

	TArray<VaCuusBundlePack::FSourceFile> Files = VaCuusBundlePack::EnumerateTree({Tree.Root});
	if (!TestEqual(TEXT("Three files enumerated"), Files.Num(), 3))
	{
		return false;
	}

	VaCuusBundleFormat::FCookedIndex IndexForward;
	TArray64<uint8> PayloadForward;
	TestTrue(TEXT("Forward pack succeeds"), VaCuusBundlePack::Pack(Files, IndexForward, PayloadForward));

	// THE double-pack: the same set, reversed -- OS enumeration order is the one
	// input a deterministic pack must provably not consume.
	TArray<VaCuusBundlePack::FSourceFile> Reversed = Files;
	Algo::Reverse(Reversed);
	VaCuusBundleFormat::FCookedIndex IndexReversed;
	TArray64<uint8> PayloadReversed;
	TestTrue(TEXT("Reversed pack succeeds"), VaCuusBundlePack::Pack(Reversed, IndexReversed, PayloadReversed));

	TestEqual(TEXT("Payload sizes match"), PayloadForward.Num(), PayloadReversed.Num());
	TestTrue(TEXT("PAYLOADS ARE BYTE-IDENTICAL across input orders"),
		PayloadForward.Num() == PayloadReversed.Num() &&
			FMemory::Memcmp(PayloadForward.GetData(), PayloadReversed.GetData(), PayloadForward.Num()) == 0);
	TestEqual(TEXT("INDEX HASHES ARE IDENTICAL across input orders"),
		VaCuusBundleFormat::HashToHex(IndexForward.ContentHash), VaCuusBundleFormat::HashToHex(IndexReversed.ContentHash));

	// The sorted-by-path layout, stated: a.rcss, b.rml, sub/c.js -- each on the
	// 64-byte alignment the format promises.
	if (TestEqual(TEXT("Three entries packed"), IndexForward.Entries.Num(), 3))
	{
		TestEqual(TEXT("Entries sort by normalized path [0]"), IndexForward.Entries[0].Path, FString(TEXT("a.rcss")));
		TestEqual(TEXT("Entries sort by normalized path [1]"), IndexForward.Entries[1].Path, FString(TEXT("b.rml")));
		TestEqual(TEXT("Entries sort by normalized path [2]"), IndexForward.Entries[2].Path, FString(TEXT("sub/c.js")));
		for (const FVaCuusBundleEntry& Entry : IndexForward.Entries)
		{
			TestEqual(FString::Printf(TEXT("'%s' is 64-byte aligned"), *Entry.Path),
				Entry.Offset % VaCuusBundleFormat::EntryAlignment, (int64)0);
		}
	}
	TestTrue(TEXT("The packed index validates against its own payload"),
		VaCuusBundleFormat::ValidateEntries(IndexForward.Entries, PayloadForward.Num(), TEXT("determinism")));

	// Negative control: determinism must not be "the hash never moves".
	TestTrue(TEXT("Fixture edited"), Tree.Write(TEXT("b.rml"), TEXT("<rml>b</rml>")));
	VaCuusBundleFormat::FCookedIndex IndexEdited;
	TArray64<uint8> PayloadEdited;
	TestTrue(TEXT("Edited pack succeeds"), VaCuusBundlePack::Pack(Files, IndexEdited, PayloadEdited));
	TestNotEqual(TEXT("One changed byte moves the content hash"),
		VaCuusBundleFormat::HashToHex(IndexEdited.ContentHash), VaCuusBundleFormat::HashToHex(IndexForward.ContentHash));

	return true;
}

/**
 * The enumeration rules (spec M6 2(a)): plugin-first duplicate-wins with every
 * shadowed file logged, the case-fold collision resolved deterministically, and
 * Tests/ fixtures excluded -- each with its count surfaced, because a rule with no
 * observable rots.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaCuusBundlePackShadowingTest, "VaCuus.Bundle.PackShadowing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaCuusBundlePackShadowingTest::RunTest(const FString& Parameters)
{
	using namespace VaCuusBundlePackTest;

	FFixtureTree First(TEXT("root_first"));
	FFixtureTree Second(TEXT("root_second"));
	if (!TestTrue(TEXT("Fixtures written"),
			First.Write(TEXT("dup.rml"), TEXT("FIRST")) &&
			First.Write(TEXT("only_first.rcss"), TEXT("1st")) &&
			First.Write(TEXT("Tests/fixture.js"), TEXT("never packed")) &&
			Second.Write(TEXT("dup.rml"), TEXT("SECOND-LONGER")) &&
			Second.Write(TEXT("only_second.rcss"), TEXT("2nd"))))
	{
		return false;
	}

	// The cross-root duplicate is logged BY the enumeration, and the message names
	// both disk paths -- the D19 stale-duplicate visibility rule, asserted rather
	// than hoped for, via a direct GLog capture (see FScopedLogCapture for why
	// AddExpectedMessage cannot do this for a Log-level line).
	FScopedLogCapture Capture;

	int32 NumShadowed = 0;
	int32 NumTestsExcluded = 0;
	const TArray<VaCuusBundlePack::FSourceFile> Files =
		VaCuusBundlePack::EnumerateTree({First.Root, Second.Root}, &NumShadowed, &NumTestsExcluded);

	TestTrue(TEXT("The shadow was LOGGED, naming the shadowed copy"),
		Capture.Contains(TEXT("dup.rml' is SHADOWED by")));

	TestEqual(TEXT("Three files survive (dup once, one per root's unique file)"), Files.Num(), 3);
	TestEqual(TEXT("One shadowed duplicate counted"), NumShadowed, 1);
	TestEqual(TEXT("One Tests/ fixture excluded and counted"), NumTestsExcluded, 1);

	const VaCuusBundlePack::FSourceFile* Dup =
		Files.FindByPredicate([](const VaCuusBundlePack::FSourceFile& File) { return File.NormalizedPath == TEXT("dup.rml"); });
	if (TestNotNull(TEXT("dup.rml enumerated once"), Dup))
	{
		TestTrue(TEXT("THE FIRST ROOT WINS the duplicate (plugin-first, D19)"), Dup->DiskPath.StartsWith(First.Root));
	}
	TestNull(TEXT("The Tests/ fixture is absent"), Files.FindByPredicate([](const VaCuusBundlePack::FSourceFile& File) {
		return File.NormalizedPath.Contains(TEXT("fixture.js"));
	}));

	// The within-root case-fold collision, deterministic tiebreak: possible only on
	// a case-sensitive filesystem, so the fixture proves it wrote two files first.
	if (First.Write(TEXT("Case.rml"), TEXT("UPPER")) && First.Write(TEXT("case.rml"), TEXT("lower")))
	{
		TArray<FString> CaseProbe;
		IFileManager::Get().FindFiles(CaseProbe, *(First.Root / TEXT("*ase.rml")), true, false);
		if (CaseProbe.Num() == 2)
		{
			int32 NumCaseShadowed = 0;
			const TArray<VaCuusBundlePack::FSourceFile> CaseFiles =
				VaCuusBundlePack::EnumerateTree({First.Root}, &NumCaseShadowed);
			TestEqual(TEXT("The case-fold pair collapsed to one"), NumCaseShadowed, 1);
			TestTrue(TEXT("The case-fold shadow was LOGGED"), Capture.Contains(TEXT("case.rml' is SHADOWED by")));
			const VaCuusBundlePack::FSourceFile* CaseWinner = CaseFiles.FindByPredicate(
				[](const VaCuusBundlePack::FSourceFile& File) { return File.NormalizedPath == TEXT("case.rml"); });
			if (TestNotNull(TEXT("The folded path enumerated once"), CaseWinner))
			{
				// ASCII order, case-SENSITIVE: 'C' (0x43) < 'c' (0x63), so Case.rml
				// wins -- and would win again on any machine, which is the point.
				TestTrue(TEXT("The lexicographically smaller disk path wins the fold"),
					CaseWinner->DiskPath.EndsWith(TEXT("/Case.rml"), ESearchCase::CaseSensitive));
			}
		}
		else
		{
			AddInfo(TEXT("Case-fold collision skipped: this filesystem folds case itself"));
		}
	}

	return true;
}

#endif	  // WITH_DEV_AUTOMATION_TESTS
