// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaCuusDocumentHost.h"
#include "VaCuusUIThread.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * WHY THIS FILE IS IN Internal/ AND NOT IN Public/ (bead VaCuus-akj.6.15).
 *
 * All four modules that carry automation tests -- VaCuus, VaCuusRender, VaCuusJs,
 * VaCuusEditor -- depend on VaCuus, so VaCuus is the only tree they can all include from.
 * Public/ would have worked and would have been wrong: VaCuusRender.Build.cs's header comment
 * is a decision record saying Public/ IS the supported C++ surface of this plugin, and a test
 * fixture is not part of that promise.
 *
 * `Internal/` is UBT's own answer and it enforces the distinction by SHAPE rather than by this
 * comment. The directory is auto-discovered with no Build.cs change (UEBuildModuleCPP.cs:451-455
 * adds it to InternalIncludePaths beside Public/ and Private/), and it reaches a consumer only
 * when `Rules.Context.Scope` matches -- UEBuildModule.cs:736-740 -- which for a plugin means
 * "another module of THIS plugin". A buyer's module gets the Public/ headers and cannot include
 * this one at all, however hard it tries. VaCuusJs reaches it despite depending on VaCuus
 * PRIVATELY, because the private/public split governs onward propagation to a dependent's own
 * consumers, not the dependent's own include path (SetupPrivateCompileEnvironment,
 * UEBuildModule.cs:810-820).
 *
 * NO RmlUi INCLUDE IN THIS FILE, deliberately: VaCuusEditor does not link VaCuusRml (VaCuus
 * keeps it a PRIVATE dependency, VaCuus.Build.cs) so the RmlUi headers do not reach it, and
 * `Rml::Context` is forward-declared by VaCuusDocumentHost.h. The real-context base lives in
 * VaCuusTestDocumentHost.h, which does include RmlUi and is therefore usable from the three
 * modules that have it. That split is the reason there are two headers rather than one.
 */

/** What FVaCuusTestNullDocumentHost::Initialize() answers. Named, because a bare bool at the
 *  construction site would read as noise at the one call that cares. */
enum class EVaCuusTestHostBoot : uint8
{
	/** Initialize() returns true: the UI thread registers the view. */
	Succeeds,

	/**
	 * Initialize() returns false. The AddView contract (VaCuusDocumentHost.h:53-55) says such a
	 * host has rolled itself back and is simply dropped without Shutdown() -- this one has
	 * nothing to roll back, which is what makes it a clean probe for the failure path
	 * (VaCuusUIThread.cpp:1479-1490 is the branch it drives).
	 */
	FailsInitialize
};

/**
 * A document host that does nothing at all -- no context, no RmlUi, no frames.
 *
 * ENOUGH, AND DELIBERATELY SO, for a test whose subject is a GAME-THREAD fact: that the reload
 * dispatcher re-issued a load, that a recompile guard fired, that the JS pump ran for a
 * REGISTERED view. Giving such a test a real context would add an RmlUi boot and a document
 * parse and assert nothing more. HasView() is false, so the UI thread never calls
 * RecordAndPublishFrame() on it (the record loop tests HasView first).
 *
 * THREAD AFFINITY: Initialize() asserts the UI thread like every other host, because that is
 * where the UI thread calls it from (VaCuusUIThread.cpp:1479) -- the other methods have no body
 * to protect.
 */
class FVaCuusTestNullDocumentHost final : public IVaCuusDocumentHost
{
public:
	explicit FVaCuusTestNullDocumentHost(EVaCuusTestHostBoot InBoot = EVaCuusTestHostBoot::Succeeds)
		: Boot(InBoot)
	{
	}

	virtual bool Initialize(uint32 /*InViewId*/, const TSharedRef<FVaCuusViewStatus>& /*InStatus*/) override
	{
		check(FVaCuusUIThread::IsInUIThread());
		return Boot == EVaCuusTestHostBoot::Succeeds;
	}

	virtual void Shutdown() override {}
	virtual void SetViewSize(FIntPoint /*ViewSize*/) override {}
	virtual void LoadDocumentFromFile(const FString& /*VfsPath*/, uint64 /*LoadSerial*/) override {}
	virtual void LoadDocumentFromMemory(const FString& /*RmlSource*/, uint64 /*LoadSerial*/) override {}
	virtual void CloseDocument() override {}
	virtual void SetVisible(bool /*bVisible*/) override {}
	virtual bool HasView() const override { return false; }
	virtual Rml::Context* GetContext() const override { return nullptr; }
	virtual void RecordAndPublishFrame() override {}

private:
	const EVaCuusTestHostBoot Boot;
};

#endif	  // WITH_DEV_AUTOMATION_TESTS
