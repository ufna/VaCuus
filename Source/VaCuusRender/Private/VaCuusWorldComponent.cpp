// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusWorldComponent.h"

#include "VaCuusDefines.h"
#include "VaCuusRmlDocumentHost.h"
#include "VaCuusSubsystem.h"
#include "VaCuusView.h"
#include "VaCuusWorldSink.h"
#include "VaCuusWorldSubsystem.h"

#include "DynamicMeshBuilder.h"
#include "Engine/GameInstance.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "MaterialShared.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialRenderProxy.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/BoxElem.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveViewRelevance.h"
#include "SceneInterface.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "TextureResource.h"
#include "UObject/ConstructorHelpers.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(VaCuusWorldComponent)

/**
 * The plane proxy, cloned from FWidget3DSceneProxy's plane branch
 * (WidgetComponent.cpp:381-411) minus what a VaCuus panel has no use for: the
 * cylinder mode, the wireframe/collision debug drawing, and the Slate renderer
 * reference (the pixels arrive through the material's texture parameter, so the
 * proxy is pure geometry). The quad spans DrawSize in world units with UV 0..1 --
 * the convention the Task 7 hit math depends on: because world units ARE pixels,
 * GetLocalHitLocation's 2D result needs no further scaling (WidgetComponent.cpp
 * :2036-2054).
 */
class FVaCuusWorldSceneProxy final : public FPrimitiveSceneProxy
{
public:
	SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	explicit FVaCuusWorldSceneProxy(UVaCuusWorldComponent* InComponent)
		: FPrimitiveSceneProxy(InComponent)
		, DrawSize(InComponent->GetCurrentDrawSize())
		, Pivot(InComponent->Pivot)
		, MaterialInstance(InComponent->GetMaterialInstance())
	{
		bWillEverBeLit = false;
		if (MaterialInstance)
		{
			MaterialRelevance = MaterialInstance->GetRelevance_Concurrent(GetScene().GetShaderPlatform());
		}
	}

	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap, FMeshElementCollector& Collector) const override
	{
		FMaterialRenderProxy* ParentMaterialProxy = MaterialInstance ? MaterialInstance->GetRenderProxy() : nullptr;
		if (!ParentMaterialProxy)
		{
			return;
		}

		const FMatrix& ViewportLocalToWorld = GetLocalToWorld();
		FMatrix PreviousLocalToWorld;
		if (!GetScene().GetPreviousLocalToWorld(GetPrimitiveSceneInfo(), PreviousLocalToWorld))
		{
			PreviousLocalToWorld = GetLocalToWorld();
		}

		// The engine's exact plane: X = 0, Y spans width, Z spans height, both offset
		// by the pivot; normal (TangentZ) +X. Same vertex order and tangent basis as
		// WidgetComponent.cpp:396-402, so the UV orientation the hit math assumes is
		// the engine's, verbatim.
		const float U = -DrawSize.X * static_cast<float>(Pivot.X);
		const float V = -DrawSize.Y * static_cast<float>(Pivot.Y);
		const float UL = DrawSize.X * (1.0f - static_cast<float>(Pivot.X));
		const float VL = DrawSize.Y * (1.0f - static_cast<float>(Pivot.Y));

		int32 VertexIndices[4];
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			if ((VisibilityMap & (1 << ViewIndex)) == 0)
			{
				continue;
			}

			FDynamicMeshBuilder MeshBuilder(Views[ViewIndex]->GetFeatureLevel());

			VertexIndices[0] = MeshBuilder.AddVertex(-FVector3f(0, U, V), FVector2f(0, 0), FVector3f(0, -1, 0), FVector3f(0, 0, -1), FVector3f(1, 0, 0), FColor::White);
			VertexIndices[1] = MeshBuilder.AddVertex(-FVector3f(0, U, VL), FVector2f(0, 1), FVector3f(0, -1, 0), FVector3f(0, 0, -1), FVector3f(1, 0, 0), FColor::White);
			VertexIndices[2] = MeshBuilder.AddVertex(-FVector3f(0, UL, VL), FVector2f(1, 1), FVector3f(0, -1, 0), FVector3f(0, 0, -1), FVector3f(1, 0, 0), FColor::White);
			VertexIndices[3] = MeshBuilder.AddVertex(-FVector3f(0, UL, V), FVector2f(1, 0), FVector3f(0, -1, 0), FVector3f(0, 0, -1), FVector3f(1, 0, 0), FColor::White);

			MeshBuilder.AddTriangle(VertexIndices[0], VertexIndices[1], VertexIndices[2]);
			MeshBuilder.AddTriangle(VertexIndices[0], VertexIndices[2], VertexIndices[3]);

			FDynamicMeshBuilderSettings Settings;
			Settings.bDisableBackfaceCulling = false;
			Settings.bReceivesDecals = false;
			Settings.bUseSelectionOutline = true;
			Settings.bIsFirstPerson = IsFirstPerson();
			MeshBuilder.GetMesh(ViewportLocalToWorld, PreviousLocalToWorld, ParentMaterialProxy, SDPG_World, Settings, nullptr, ViewIndex, Collector, FHitProxyId());
		}
	}

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result;
		MaterialRelevance.SetPrimitiveViewRelevance(Result);
		Result.bDrawRelevance = IsShown(View);
		Result.bDynamicRelevance = true;
		Result.bRenderCustomDepth = ShouldRenderCustomDepth();
		Result.bRenderInMainPass = ShouldRenderInMainPass();
		Result.bShadowRelevance = IsShadowCast(View);
		Result.bEditorPrimitiveRelevance = false;
		return Result;
	}

	virtual bool CanBeOccluded() const override { return !MaterialRelevance.bDisableDepthTest; }
	virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }

private:
	FIntPoint DrawSize;
	FVector2D Pivot;
	UMaterialInstanceDynamic* MaterialInstance;
	FMaterialRelevance MaterialRelevance;
};

UVaCuusWorldComponent::UVaCuusWorldComponent()
{
	// NEVER TICKS -- see the class comment: the sink is arrival-driven and the
	// material binds a UTexture whose FRHITextureReference follows every resource
	// recreation on its own (the uniform-expression path binds
	// TextureReference.TextureReferenceRHI, MaterialUniformExpressions.cpp:1708-1729).
	PrimaryComponentTick.bCanEverTick = false;

	// The engine's own world-UI profile (WidgetComponent.cpp:653): blocks the
	// Visibility traces the Task 7 processor and GetHitResultAtScreenPosition use.
	BodyInstance.SetCollisionProfileName(FName("UI"));

	// One preset instead of UWidgetComponent's six (spec 2(i)); authored by
	// docs/research/proofs/m5-t6-worldspace/author_world_panel_material.py and
	// committed, because runtime-constructed UMaterials cannot compile outside the
	// editor (the Task 5 spike's finding (2), spec 3.3). This hard reference is also
	// what cooks it -- no DirectoriesToAlwaysCook entry needed.
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> PresetFinder(TEXT("/VaCuus/M_VaCuusWorldPanel"));
	PresetMaterial = PresetFinder.Object;
}

void UVaCuusWorldComponent::OnRegister()
{
	// Before Super, so the first CalcBounds already sees real extents -- the engine's
	// own ordering note (WidgetComponent.cpp:950-955).
	CurrentDrawSize = DrawSize;

	Super::OnRegister();

	const UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld())
	{
		// Editor preview worlds get geometry-less registration and no view -- the same
		// gate as UVaCuusWorldSubsystem (game sessions only), and for the same reason
		// the UMG widget refuses at design time: no game instance owns a view here.
		return;
	}

	// THE NAMED REFUSAL (spec 2(h)): one Error, no view. Deliberately NOT the screen
	// path's silent zero -- there, zero means "UMG has not laid us out yet" and the
	// first tick heals it (VaCuusUMGWidget.cpp:69-76); a world panel's size is this
	// property and nothing will ever arrive to correct it, so waiting would just hide
	// the misconfiguration. The replayer would also skip every draw at this size
	// (VaCuusReplayRenderer.cpp:157-163), so "refuse loudly" costs nothing over
	// "record into nothing forever".
	if (DrawSize.X <= 0 || DrawSize.Y <= 0)
	{
		UE_LOG(LogVaCuus, Error,
			TEXT("VaCuus world panel '%s' refused: degenerate DrawSize %dx%d. A world panel's pixel size is DrawSize itself ")
			TEXT("(there is no layout pass to wait for); set it positive before registering"),
			*GetName(), DrawSize.X, DrawSize.Y);
		return;
	}

	if (View == nullptr)
	{
		CreateView();
	}
	if (View == nullptr)
	{
		return;
	}

	UpdateRenderTarget();
	UpdateMaterialInstance();

	// The Task 7 seam: the input processor's roster (and its refcounted install).
	if (UVaCuusWorldSubsystem* WorldSubsystem = GetWorld()->GetSubsystem<UVaCuusWorldSubsystem>())
	{
		WorldSubsystem->RegisterWorldComponent(this);
	}

	if (bAutoLoadDocument && !DocumentPath.IsEmpty() && DocumentPath != AppliedDocumentPath)
	{
		AppliedDocumentPath = DocumentPath;
		View->LoadDocument(DocumentPath);
	}
}

void UVaCuusWorldComponent::OnUnregister()
{
	if (UWorld* World = GetWorld())
	{
		if (UVaCuusWorldSubsystem* WorldSubsystem = World->GetSubsystem<UVaCuusWorldSubsystem>())
		{
			WorldSubsystem->UnregisterWorldComponent(this);
		}
	}

	RetireView();

	// Dropped so a re-register rebuilds all three from scratch: a NEW sink starts with
	// an empty destination slot, and only UpdateRenderTarget's create path enqueues
	// the slot update -- keeping the old RT across the gap would leave the new sink
	// destination-less at an unchanged size. Render commands still in flight hold
	// their own refs to the old sink and old resources.
	Sink.Reset();
	RenderTarget = nullptr;
	MaterialInstance = nullptr;
	AppliedDocumentPath.Reset();

	Super::OnUnregister();
}

void UVaCuusWorldComponent::CreateView()
{
	UWorld* World = GetWorld();
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	UVaCuusSubsystem* Subsystem = GameInstance ? GameInstance->GetSubsystem<UVaCuusSubsystem>() : nullptr;
	if (!Subsystem)
	{
		UE_LOG(LogVaCuus, Warning, TEXT("VaCuus world panel '%s' found no UVaCuusSubsystem, so it has nothing to show"),
			*GetName());
		return;
	}

	const TSharedRef<FVaCuusWorldSink> NewSink = MakeShared<FVaCuusWorldSink>();

	// DrawSize UP FRONT, unlike the UMG widget's deliberate zero (its size is a layout
	// fact it does not have yet, VaCuusUMGWidget.cpp:69-76); ours is a property. The
	// context is laid out at the real size before the first recorded frame.
	UVaCuusView* NewView = Subsystem->CreateView(MakeUnique<FVaCuusRmlDocumentHost>(NewSink), CurrentDrawSize);
	if (!NewView)
	{
		// Logged in detail by the subsystem/module. The sink never touched the RHI.
		return;
	}

	View = NewView;
	OwningSubsystem = Subsystem;
	Sink = NewSink;
}

void UVaCuusWorldComponent::RetireView()
{
	UVaCuusView* ViewPtr = View;
	View = nullptr;
	if (!ViewPtr)
	{
		return;
	}

	// If the subsystem is already gone (world or engine teardown got here first) it
	// has invalidated the view for us -- the UMG widget's exact teardown contract
	// (VaCuusUMGWidget.cpp:268-274). DestroyView drains to the host's Shutdown on the
	// UI thread, which enqueues the sink's ReleaseResources_RenderThread ordered
	// after the view's last publish.
	if (UVaCuusSubsystem* Subsystem = OwningSubsystem.Get())
	{
		Subsystem->DestroyView(ViewPtr);
	}
	OwningSubsystem = nullptr;
}

void UVaCuusWorldComponent::SetDrawSize(FIntPoint NewDrawSize)
{
	// The same named refusal as OnRegister, for the same reason: nothing heals it.
	if (NewDrawSize.X <= 0 || NewDrawSize.Y <= 0)
	{
		UE_LOG(LogVaCuus, Error, TEXT("VaCuus world panel '%s' refused SetDrawSize(%d, %d): degenerate size, keeping %dx%d"),
			*GetName(), NewDrawSize.X, NewDrawSize.Y, CurrentDrawSize.X, CurrentDrawSize.Y);
		return;
	}

	DrawSize = NewDrawSize;
	if (CurrentDrawSize == NewDrawSize || View == nullptr)
	{
		// Not registered into a game world yet (or no change): OnRegister applies it.
		return;
	}
	CurrentDrawSize = NewDrawSize;

	// Order matters only in that the RT re-init precedes ITS slot-update enqueue
	// (inside UpdateRenderTarget -- the FIFO half of spec 2(g)); the view's relayout
	// races both by design and every interleaving lands on the sink's extent guard.
	View->Resize(CurrentDrawSize);
	UpdateRenderTarget();
	UpdateBodySetup(/*bDrawSizeChanged=*/true);
	RecreatePhysicsState();
	MarkRenderStateDirty();
	UpdateBounds();
}

void UVaCuusWorldComponent::SetTwoSided(bool bInTwoSided)
{
	bTwoSided = bInTwoSided;
	if (MaterialInstance)
	{
		// A scalar, not a static switch -- see bTwoSided's declaration for why a MID
		// cannot do this any other way at runtime.
		MaterialInstance->SetScalarParameterValue(TEXT("VaCuusBackfaceOpacity"), bTwoSided ? 1.0f : 0.0f);
	}
}

void UVaCuusWorldComponent::LoadDocument(FString Path)
{
	// Kept even without a view, so a load requested before registration is applied by
	// OnRegister when the view exists.
	DocumentPath = MoveTemp(Path);

	if (View == nullptr)
	{
		return;
	}

	AppliedDocumentPath = DocumentPath;
	if (DocumentPath.IsEmpty())
	{
		View->Close();
	}
	else
	{
		View->LoadDocument(DocumentPath);
	}
}

void UVaCuusWorldComponent::Close()
{
	AppliedDocumentPath.Reset();
	if (View)
	{
		View->Close();
	}
}

FPrimitiveSceneProxy* UVaCuusWorldComponent::CreateSceneProxy()
{
	// No MID means no game-world view (editor preview, refused size, no subsystem):
	// nothing to draw with. Editor visualization of an unregistered panel is not a
	// v1 concern -- the component is expected to be spawned, not level-authored.
	if (MaterialInstance == nullptr || CurrentDrawSize.X <= 0 || CurrentDrawSize.Y <= 0)
	{
		return nullptr;
	}
	return new FVaCuusWorldSceneProxy(this);
}

FBoxSphereBounds UVaCuusWorldComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	// The engine's world-space branch verbatim (WidgetComponent.cpp:907-924).
	const FVector Origin = FVector(.5f,
		-(CurrentDrawSize.X * 0.5f) + (CurrentDrawSize.X * Pivot.X),
		-(CurrentDrawSize.Y * 0.5f) + (CurrentDrawSize.Y * Pivot.Y));
	const FVector BoxExtent = FVector(1.f, CurrentDrawSize.X / 2.0f, CurrentDrawSize.Y / 2.0f);

	FBoxSphereBounds NewBounds(Origin, BoxExtent, CurrentDrawSize.Size() / 2.0f);
	NewBounds = NewBounds.TransformBy(LocalToWorld);
	NewBounds.BoxExtent *= BoundsScale;
	NewBounds.SphereRadius *= BoundsScale;
	return NewBounds;
}

UBodySetup* UVaCuusWorldComponent::GetBodySetup()
{
	UpdateBodySetup();
	return BodySetup;
}

FCollisionShape UVaCuusWorldComponent::GetCollisionShape(float Inflation) const
{
	// WidgetComponent.cpp:937-943.
	const FVector BoxHalfExtent =
		FVector(0.01f, CurrentDrawSize.X * 0.5f, CurrentDrawSize.Y * 0.5f) * GetComponentTransform().GetScale3D();
	return FCollisionShape::MakeBox(BoxHalfExtent).Inflate(Inflation);
}

void UVaCuusWorldComponent::SetMaterial(int32 ElementIndex, UMaterialInterface* Material)
{
	// Same shape as the engine's override hook (WidgetComponent.cpp:778-783): the
	// override lands in OverrideMaterials via Super, then the MID is rebuilt over it.
	// An override material owes the same parameter names the preset carries
	// (VaCuusUI, VaCuusBackfaceOpacity) and the AlphaComposite blend for correct
	// results; that contract is on the preset's doc, not enforced here.
	Super::SetMaterial(ElementIndex, Material);
	UpdateMaterialInstance();
}

UMaterialInterface* UVaCuusWorldComponent::GetMaterial(int32 MaterialIndex) const
{
	if (OverrideMaterials.IsValidIndex(MaterialIndex) && OverrideMaterials[MaterialIndex] != nullptr)
	{
		return OverrideMaterials[MaterialIndex];
	}
	return MaterialIndex == 0 ? PresetMaterial : nullptr;
}

void UVaCuusWorldComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	// The MID, not GetMaterial(0): the proxy draws with the instance, and the render
	// thread's used-material verification checks against this list
	// (WidgetComponent.cpp:1618-1624 does the same).
	if (MaterialInstance)
	{
		OutMaterials.AddUnique(MaterialInstance);
	}
}

void UVaCuusWorldComponent::UpdateRenderTarget()
{
	if (!Sink.IsValid() || CurrentDrawSize.X <= 0 || CurrentDrawSize.Y <= 0)
	{
		return;
	}

	// UWidgetComponent::UpdateRenderTarget's create/resize shape (WidgetComponent.cpp
	// :1962-1988) with ONE deliberate divergence: bForceLinearGamma = true where the
	// engine passes false. For an override format IsSRGB() is !bForceLinearGamma
	// (TextureRenderTarget2D.cpp:72-87), and this RT must NOT be sRGB-tagged: it
	// receives a raw CopyTexture from the replay RT, which is created without
	// TexCreate_SRGB and holds display-encoded premultiplied pixels
	// (VaCuusReplayRenderer.cpp:170-178). An sRGB-tagged destination would make the
	// material's sampler hardware-decode pixels the WS-GAMMA decision decodes (or
	// not) explicitly in the preset's graph.
	bool bChanged = false;
	if (RenderTarget == nullptr)
	{
		RenderTarget = NewObject<UTextureRenderTarget2D>(this);
		RenderTarget->ClearColor = FLinearColor::Transparent;
		RenderTarget->InitCustomFormat(CurrentDrawSize.X, CurrentDrawSize.Y, PF_B8G8R8A8, /*bInForceLinearGamma=*/true);
		bChanged = true;
	}
	else if (RenderTarget->SizeX != CurrentDrawSize.X || RenderTarget->SizeY != CurrentDrawSize.Y)
	{
		RenderTarget->InitCustomFormat(CurrentDrawSize.X, CurrentDrawSize.Y, PF_B8G8R8A8, /*bInForceLinearGamma=*/true);
		RenderTarget->UpdateResourceImmediate();
		bChanged = true;
	}

	if (bChanged)
	{
		// THE SLOT-UPDATE ENQUEUE, after EVERY (re)init and from this thread only --
		// the FIFO half of spec 2(g)'s discipline: the (re)init above already enqueued
		// the resource's own recreation, so by the time this command resolves the
		// resource's RHI texture on the render thread, it is the NEW one. The resource
		// pointer is captured raw, the engine's own pattern for render targets: an
		// FTextureResource is deleted through deferred render-thread cleanup, so it
		// outlives every command enqueued before its release.
		FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
		ENQUEUE_RENDER_COMMAND(VaCuusWorldSetDestination)(
			[LocalSink = Sink, Resource](FRHICommandListImmediate& RHICmdList)
			{
				LocalSink->SetDestination_RenderThread(RHICmdList, Resource ? Resource->GetRenderTargetTexture() : nullptr);
			});

		if (MaterialInstance)
		{
			// Re-pushed on recreation exactly like the engine's SlateUI param
			// (WidgetComponent.cpp:1971-1974); the parameter itself survives resizes
			// (the UTexture is stable, only its resource moved) but not the
			// RenderTarget == nullptr create path.
			MaterialInstance->SetTextureParameterValue(TEXT("VaCuusUI"), RenderTarget);
		}
		MarkRenderStateDirty();
	}
}

void UVaCuusWorldComponent::UpdateMaterialInstance()
{
	MaterialInstance = nullptr;

	UMaterialInterface* BaseMaterial = GetMaterial(0);
	if (BaseMaterial == nullptr)
	{
		// The preset failed to load AND no override: the panel will be invisible;
		// the ConstructorHelpers finder already logged the missing asset.
		return;
	}

	// The engine's shape (WidgetComponent.cpp:785-808) minus the cluster bookkeeping
	// this component does not opt into (CanBeInCluster is not overridden here).
	MaterialInstance = UMaterialInstanceDynamic::Create(BaseMaterial, this);
	if (RenderTarget)
	{
		MaterialInstance->SetTextureParameterValue(TEXT("VaCuusUI"), RenderTarget);
	}
	MaterialInstance->SetScalarParameterValue(TEXT("VaCuusBackfaceOpacity"), bTwoSided ? 1.0f : 0.0f);

	MarkRenderStateDirty();
}

void UVaCuusWorldComponent::UpdateBodySetup(bool bDrawSizeChanged)
{
	// WidgetComponent.cpp:2006-2033, world-space branch: a hand-built box, 0.01uu
	// thick, simple-as-complex, spanning DrawSize around the pivot.
	if (!BodySetup || bDrawSizeChanged)
	{
		BodySetup = NewObject<UBodySetup>(this);
		BodySetup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
		BodySetup->AggGeom.BoxElems.Add(FKBoxElem());

		FKBoxElem* BoxElem = BodySetup->AggGeom.BoxElems.GetData();

		const double Width = CurrentDrawSize.X;
		const double Height = CurrentDrawSize.Y;
		const FVector Origin = FVector(.5f,
			-(Width * 0.5f) + (Width * Pivot.X),
			-(Height * 0.5f) + (Height * Pivot.Y));

		BoxElem->X = 0.01f;
		BoxElem->Y = static_cast<float>(Width);
		BoxElem->Z = static_cast<float>(Height);

		BoxElem->SetTransform(FTransform::Identity);
		BoxElem->Center = Origin;
	}
}
