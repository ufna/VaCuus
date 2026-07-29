// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusEngine.h"

#include "VaCuus.h"
#include "VaCuusDefines.h"
#include "VaCuusFileInterface.h"
#include "VaCuusSystemInterface.h"

#include "HAL/PlatformTLS.h"
#include "Misc/Paths.h"

#include <RmlUi/Core.h>

/**
 * Render interface stub that satisfies RmlUi without touching any RHI.
 * Hands out dummy non-zero handles so layout and document loading work
 * headlessly (tests, commandlets). Replaced by the real RHI backend later.
 */
class FVaCuusNullRenderInterface : public Rml::RenderInterface
{
public:
	//~ Begin Rml::RenderInterface
	virtual Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> Vertices, Rml::Span<const int> Indices) override
	{
		return Rml::CompiledGeometryHandle(NextHandle++);
	}

	virtual void RenderGeometry(Rml::CompiledGeometryHandle Geometry, Rml::Vector2f Translation, Rml::TextureHandle Texture) override {}
	virtual void ReleaseGeometry(Rml::CompiledGeometryHandle Geometry) override {}

	virtual Rml::TextureHandle LoadTexture(Rml::Vector2i& TextureDimensions, const Rml::String& Source) override
	{
		TextureDimensions = Rml::Vector2i(1, 1);
		return Rml::TextureHandle(NextHandle++);
	}

	virtual Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> Source, Rml::Vector2i SourceDimensions) override
	{
		return Rml::TextureHandle(NextHandle++);
	}

	virtual void ReleaseTexture(Rml::TextureHandle Texture) override {}

	virtual void EnableScissorRegion(bool bEnable) override {}
	virtual void SetScissorRegion(Rml::Rectanglei Region) override {}
	//~ End Rml::RenderInterface

private:
	uintptr_t NextHandle = 1;
};

FVaCuusEngine::FVaCuusEngine() = default;
FVaCuusEngine::~FVaCuusEngine() = default;

FVaCuusEngine& FVaCuusEngine::Get()
{
	// Module-owned: torn down in FVaCuusModule::ShutdownModule(), i.e. before
	// static destruction and before VaCuusRml unloads. Loading on demand keeps
	// early callers (automation tests, other modules) working.
	return FVaCuusModule::Get().GetEngine();
}

bool FVaCuusEngine::Initialize()
{
	// Claim ownership atomically: exactly one thread can move OwnerThreadId from 0
	// to its own id, so only one thread ever reaches the boot below even if several
	// race the first Initialize(). Testing "RefCount > 0" instead would be
	// check-then-act: two threads could both see 0, both pass the owner check and
	// both call Rml::Initialise().
	uint32 ExpectedOwner = 0;
	const bool bClaimedOwnership = OwnerThreadId.compare_exchange_strong(
		ExpectedOwner, FPlatformTLS::GetCurrentThreadId(),
		std::memory_order_acq_rel, std::memory_order_acquire);

	if (!bClaimedOwnership)
	{
		// Somebody already owns the library. If it is us this is a nested
		// Initialize() and just adds a reference; if it is another thread, this is
		// the contract violation the owner record exists to catch.
		CheckOwnerThread(TEXT("Initialize"));
		RefCount.fetch_add(1, std::memory_order_acq_rel);
		return true;
	}

	SystemInterface = MakeUnique<FVaCuusSystemInterface>();
	FileInterface = MakeUnique<FVaCuusFileInterface>();

	if (RenderInterface == nullptr)
	{
		NullRenderInterface = MakeUnique<FVaCuusNullRenderInterface>();
		RenderInterface = NullRenderInterface.Get();
	}

	Rml::SetSystemInterface(SystemInterface.Get());
	Rml::SetFileInterface(FileInterface.Get());
	Rml::SetRenderInterface(RenderInterface);

	if (!Rml::Initialise())
	{
		UE_LOG(LogVaCuus, Error, TEXT("Rml::Initialise() failed"));

		// RmlUi caches these as raw static pointers and only clears them in
		// Rml::Shutdown(); null them out before destroying the interfaces so
		// nothing is left dangling after this failure path.
		Rml::SetSystemInterface(nullptr);
		Rml::SetFileInterface(nullptr);
		Rml::SetRenderInterface(nullptr);

		// Drop only our stub; an externally provided interface is kept for a later retry.
		if (RenderInterface == NullRenderInterface.Get())
		{
			RenderInterface = nullptr;
		}
		NullRenderInterface.Reset();
		SystemInterface.Reset();
		FileInterface.Reset();

		// Release the claim so a later retry can boot: this thread owns the record
		// but never took a reference, and RefCount is still 0.
		OwnerThreadId.store(0, std::memory_order_release);
		return false;
	}

	// Default font face; missing font is a warning, not a boot failure.
	const FString FontDiskPath = FPaths::ProjectContentDir() / TEXT("DevUI/fonts/LatoLatin-Regular.ttf");
	if (FPaths::FileExists(FontDiskPath))
	{
		// Relative path: resolved by FVaCuusFileInterface against <Project>/Content/DevUI/.
		if (!Rml::LoadFontFace("fonts/LatoLatin-Regular.ttf"))
		{
			UE_LOG(LogVaCuus, Warning, TEXT("Failed to load default font face from '%s'"), *FontDiskPath);
		}
	}
	else
	{
		UE_LOG(LogVaCuus, Warning, TEXT("Default font not found at '%s'; text will not be rendered"), *FontDiskPath);
	}

	// Published last: a reader that sees a non-zero refcount sees a booted library.
	RefCount.store(1, std::memory_order_release);

	UE_LOG(LogVaCuus, Log, TEXT("RmlUi initialized (owner thread %u)"),
		OwnerThreadId.load(std::memory_order_relaxed));
	return true;
}

void FVaCuusEngine::Shutdown()
{
	CheckOwnerThread(TEXT("Shutdown"));

	if (RefCount.load(std::memory_order_acquire) == 0)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("Shutdown() called without a matching Initialize()"));
		return;
	}

	// fetch_sub returns the value *before* the decrement, so >1 means somebody else
	// still holds a reference.
	if (RefCount.fetch_sub(1, std::memory_order_acq_rel) > 1)
	{
		return;
	}

	TearDownLibrary();
	UE_LOG(LogVaCuus, Log, TEXT("RmlUi shut down"));
}

bool FVaCuusEngine::IsClaimableOnThisThread() const
{
	const uint32 Owner = OwnerThreadId.load(std::memory_order_acquire);
	return Owner == 0 || Owner == FPlatformTLS::GetCurrentThreadId();
}

void FVaCuusEngine::ForceShutdownAll()
{
	// Deliberately skips CheckOwnerThread(): this is the module-unload escape hatch
	// for an unpaired Initialize(), and the owner may be a thread that is already
	// gone. Tearing RmlUi down from the wrong thread beats leaking the library past
	// the point where VaCuusRml is still loaded. Since FVaCuusModule stops the UI
	// thread it owns before it gets here, reaching this function means some *other*
	// holder went unpaired -- it is a diagnostic, not a step.
	if (RefCount.load(std::memory_order_acquire) == 0)
	{
		return;
	}

	// Drops every outstanding reference at once; a later Shutdown() from whoever
	// held one will report the unpaired call, which is the truth.
	RefCount.store(0, std::memory_order_release);
	TearDownLibrary();
	UE_LOG(LogVaCuus, Log, TEXT("RmlUi shut down (forced)"));
}

void FVaCuusEngine::TearDownLibrary()
{
	Rml::Shutdown();

	// Interfaces must outlive Rml::Shutdown(); safe to drop them now.
	if (RenderInterface == NullRenderInterface.Get())
	{
		RenderInterface = nullptr;
	}
	NullRenderInterface.Reset();
	SystemInterface.Reset();
	FileInterface.Reset();

	// Released last, and only now: the claim is what gates the next boot, so holding
	// it until Rml::Shutdown() has returned is what stops a new owner from calling
	// Rml::Initialise() while this teardown is still in flight.
	OwnerThreadId.store(0, std::memory_order_release);
}

void FVaCuusEngine::SetRenderInterface(Rml::RenderInterface* InRenderInterface)
{
	CheckOwnerThread(TEXT("SetRenderInterface"));

	if (RefCount.load(std::memory_order_acquire) > 0)
	{
		UE_LOG(LogVaCuus, Error, TEXT("SetRenderInterface() must be called before Initialize(); ignored"));
		return;
	}

	RenderInterface = InRenderInterface;
	NullRenderInterface.Reset();
}

void FVaCuusEngine::CheckOwnerThread(const TCHAR* Operation) const
{
	// Nobody owns the library between teardown and the next boot, which is exactly
	// when SetRenderInterface() is supposed to be called -- so an unowned engine
	// accepts calls from anywhere and the *next* Initialize() picks the owner.
	const uint32 Owner = OwnerThreadId.load(std::memory_order_acquire);
	checkf(Owner == 0 || Owner == FPlatformTLS::GetCurrentThreadId(),
		TEXT("FVaCuusEngine::%s() called from thread %u, but RmlUi is owned by thread %u"),
		Operation, FPlatformTLS::GetCurrentThreadId(), Owner);
}
