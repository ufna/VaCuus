// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Components/MeshComponent.h"

#include "VaCuusWorldComponent.generated.h"

// FVaCuusWorldSink stays in Private/ and stays FORWARD-DECLARED here; see
// GetWorldSink() for what that costs and why it is the right trade.
class FVaCuusWorldSink;
class UBodySetup;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UTextureRenderTarget2D;
class UVaCuusSubsystem;
class UVaCuusView;

/**
 * A VaCuus view on a quad in the world (M5 spec 3.4): DrawSize pixels rendered by
 * the shared UI thread, replayed and copied into this component's render target on
 * every PUBLISHED frame, and drawn into the scene by the one preset material
 * (/VaCuus/M_VaCuusWorldPanel, BLEND_AlphaComposite -- spec 2(i)).
 *
 * WHAT IT SHARES WITH THE SCREEN PATH: everything up to the sink. The component
 * creates its view exactly as UVaCuusWidget does -- one FVaCuusRmlDocumentHost into
 * UVaCuusSubsystem::CreateView -- except the host publishes to an FVaCuusWorldSink
 * instead of a Slate element, and the size is passed UP FRONT: a world panel's pixel
 * size IS DrawSize, known at creation, so there is none of the UMG widget's
 * deliberate zero-size first-tick dance (VaCuusUMGWidget.cpp:69-76).
 *
 * WHAT IT CLONES FROM UWidgetComponent, with the citations opened: the pixel-sized
 * plane proxy (WidgetComponent.cpp:381-411 -- DrawSize world units, UV 0..1), the
 * hand-built UBodySetup box (:2006-2033, 0.01uu thick, CTF_UseSimpleAsComplex),
 * collision profile "UI" (:653), CalcBounds (:907-929) and GetCollisionShape
 * (:937-948). What it deliberately does NOT clone: the cylinder mode, the
 * SVirtualWindow/FWidgetRenderer machinery (there is no widget -- the pixels already
 * exist per view), the six-material matrix (:656-671; our RT's premultiplied
 * contract wants exactly one blend state), and the per-frame tick -- this component
 * NEVER TICKS; the sink is arrival-driven and the material samples a UTexture whose
 * reference survives every resource recreation.
 *
 * COST MODEL (WS-COPY-COST): one GPU copy per PUBLISHED frame -- ~never while the
 * document is idle (the M2 idle gate; 2 publishes in 13,074 recorded frames on the
 * static M1 HUD). THE EXCEPTION IS A MATERIAL-DECORATOR DOCUMENT: a live
 * `decorator: shader(<StyleSet key>)` forces republish clamped to engine rate (M5
 * Task 5b remedy), so such a panel copies once per engine frame -- budget
 * accordingly, or keep material decorators off world panels.
 *
 * ZERO/DEGENERATE DrawSize IS A NAMED REFUSAL at OnRegister (spec 2(h)): one Error,
 * no view, and no first-tick heal by design -- the screen path's zero size means
 * "layout has not arrived yet", a world panel's means the component is misconfigured
 * and waiting would hide it.
 *
 * INPUT (M5 Task 7): OnRegister/OnUnregister maintain UVaCuusWorldSubsystem's
 * roster AND refcount the process-wide FVaCuusWorldInputProcessor -- the first
 * panel anywhere installs it, the last one out uninstalls it (the processor's
 * header carries the whole design: the occlusion rule, the trace, the latch).
 * The panel hears pointer events only; keys and IME stay with the screen path
 * (decision D17).
 */
/**
 * VACUUSRENDER_API: the same VaCuus-dgl defect as UVaCuusWidget, found by the same audit
 * -- this class is BlueprintSpawnableComponent and perf-guide.md:107,113 tells buyers to
 * set bGenerateMips / call SetGenerateMips() on it, but with the header in Private/ and no
 * export macro, a buyer with precompiled binaries could only reach it from Blueprint.
 * `CreateDefaultSubobject<UVaCuusWorldComponent>` in an actor's constructor -- the ordinary
 * way anyone adds a component from C++ -- could neither include the header nor link. The
 * whole mechanism (why the UCLASS still registered) is on UVaCuusWidget.
 */
UCLASS(Blueprintable, ClassGroup = "UserInterface", meta = (BlueprintSpawnableComponent, DisplayName = "VaCuus World Panel"),
	hidecategories = (Object, Activation, "Components|Activation", Sockets, Base, Lighting, LOD, Mesh))
class VACUUSRENDER_API UVaCuusWorldComponent : public UMeshComponent
{
	GENERATED_BODY()

public:
	UVaCuusWorldComponent();

	/**
	 * The panel's pixel size -- the view's layout size, the render target's extent
	 * AND the quad's unscaled world-unit extent, exactly UWidgetComponent's
	 * convention (pixels as units; scale the component for world sizing). Runtime
	 * changes go through SetDrawSize().
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VaCuus")
	FIntPoint DrawSize = FIntPoint(1024, 512);

	/** Where the origin sits on the quad, 0..1; (0.5, 0.5) = centered. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VaCuus")
	FVector2D Pivot = FVector2D(0.5, 0.5);

	/** Document to show (VFS path against the DevUI roots). Empty = load later. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaCuus", meta = (DisplayName = "Document"))
	FString DocumentPath;

	/** Load DocumentPath as soon as the view exists. Off means the game decides when. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaCuus")
	bool bAutoLoadDocument = true;

	/**
	 * Draw the back face too (mirrored, as every two-sided UI is). Runtime-settable
	 * through SetTwoSided(): the preset material is two-sided at the MATERIAL level
	 * and kills the back face with the VaCuusBackfaceOpacity scalar instead of a
	 * static switch, because a UMaterialInstanceDynamic cannot set static switches
	 * at runtime (no such setter exists on it, and the permutation it would need is
	 * editor-compiled only -- the Task 5 spike's recorded finding (2), spec 3.3).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VaCuus")
	bool bTwoSided = true;

	/**
	 * Give the render target a full mip chain, regenerated after every PUBLISHED
	 * frame, and sample it trilinearly. ON buys a panel that stays readable instead of
	 * strobing when it shrinks on screen (distance, glancing angles): minification
	 * samples a filtered far mip instead of skipping texels. It costs one
	 * FGenerateMips pass per published frame (the PerfLog's WorldMips line -- ~zero
	 * while idle, per engine frame with a live material decorator, exactly like the
	 * copy) and ~33% more render-target memory. Turn it OFF for a panel that never
	 * minifies -- always near-1:1 on screen -- or to shave the memory/publish cost;
	 * runtime changes go through SetGenerateMips().
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "VaCuus")
	bool bGenerateMips = true;

	/** Re-sizes the view, render target, quad and collision. Degenerate sizes are refused with an Error. */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	void SetDrawSize(FIntPoint NewDrawSize);

	/** See bTwoSided. */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	void SetTwoSided(bool bInTwoSided);

	/** See bGenerateMips. Re-inits the render target (same shape as a resize); the panel repaints itself. */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	void SetGenerateMips(bool bInGenerateMips);

	/** Replaces the current document (asynchronous, like everything on the view). */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	void LoadDocument(FString Path);

	/** Closes the document. The view stays alive and can load another. */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	void Close();

	/** This panel's view handle, or null (editor world, refused DrawSize, not registered). */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	UVaCuusView* GetView() const { return View; }

	/** The render target the sink copies into; what the material's VaCuusUI parameter samples. */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	UTextureRenderTarget2D* GetRenderTarget() const { return RenderTarget; }

	/** The MID over the preset (or an override set via SetMaterial(0, ...)). */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	UMaterialInstanceDynamic* GetMaterialInstance() const { return MaterialInstance; }

	/** The pixel size everything below is currently built at (== DrawSize once registered). */
	FIntPoint GetCurrentDrawSize() const { return CurrentDrawSize; }

	/**
	 * Diagnostics/tests: the sink and its copy/skip counters. Null before registration.
	 *
	 * DELIBERATELY NOT PART OF THE SUPPORTED SURFACE, even though it is declared in a
	 * public header. FVaCuusWorldSink is the render backend's own type and stays in
	 * Private/; a buyer can call this and hold the handle (TSharedPtr needs no complete
	 * type -- see the note over UVaCuusWidget's members) but cannot do anything with it,
	 * which is the intent. It is here because the world-panel tests are in this module and
	 * the counters are WS-COPY-COST's only observable; exporting the sink to make it
	 * usable would drag FVaCuusReplayRenderer's whole command-buffer contract into the
	 * supported ABI for no buyer-facing gain (VaCuusRender.Build.cs carries that decision).
	 */
	TSharedPtr<FVaCuusWorldSink> GetWorldSink() const { return Sink; }

	//~ Begin UActorComponent
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	//~ End UActorComponent

	//~ Begin USceneComponent
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	//~ End USceneComponent

	//~ Begin UPrimitiveComponent
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual UBodySetup* GetBodySetup() override;
	virtual FCollisionShape GetCollisionShape(float Inflation) const override;
	virtual void SetMaterial(int32 ElementIndex, UMaterialInterface* Material) override;
	//~ End UPrimitiveComponent

	//~ Begin UMeshComponent
	virtual UMaterialInterface* GetMaterial(int32 MaterialIndex) const override;
	virtual int32 GetNumMaterials() const override { return 1; }
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;
	//~ End UMeshComponent

private:
	/** Creates the sink + host + view (DrawSize up front) or refuses. Game worlds only. */
	void CreateView();

	/** Mirrors UVaCuusWidget::RetireView(): DestroyView -> the host's Shutdown releases the sink. */
	void RetireView();

	/**
	 * Creates or resizes the render target and -- after EVERY (re)init -- enqueues
	 * the sink's destination-slot update, FIFO with the resource recreation it
	 * follows (spec 2(g); the whole discipline is on FVaCuusWorldSink).
	 */
	void UpdateRenderTarget();

	/** MID over GetMaterial(0); binds VaCuusUI + the backface scalar. */
	void UpdateMaterialInstance();

	/** The UBodySetup box clone (WidgetComponent.cpp:2006-2033). */
	void UpdateBodySetup(bool bDrawSizeChanged = false);

	/** This panel's view; owned by the subsystem, referenced here. */
	UPROPERTY(Transient)
	TObjectPtr<UVaCuusView> View;

	/** Who to retire the view with. Weak: world teardown may get there first. */
	UPROPERTY(Transient)
	TWeakObjectPtr<UVaCuusSubsystem> OwningSubsystem;

	/** The copy destination; non-sRGB PF_B8G8R8A8, extent == CurrentDrawSize, full mip chain iff bGenerateMips. */
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> RenderTarget;

	/** See GetMaterialInstance(). */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> MaterialInstance;

	/** See UpdateBodySetup(). */
	UPROPERTY(Transient)
	TObjectPtr<UBodySetup> BodySetup;

	/** The shipped preset (ConstructorHelpers); base of the MID unless overridden. */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> PresetMaterial;

	/** The render-thread half; created with the view, released by the host's Shutdown. */
	TSharedPtr<FVaCuusWorldSink> Sink;

	/** DrawSize as actually applied (registration snapshots it, SetDrawSize moves it). */
	FIntPoint CurrentDrawSize = FIntPoint::ZeroValue;

	/**
	 * True between AddInstallRef and ReleaseInstallRef, so the pair cannot go
	 * unbalanced across the refusal paths: a refused or view-less registration never
	 * takes the ref and must never release one.
	 */
	bool bHoldsInputProcessorRef = false;

	/** Guards the auto-load against double delivery, same idea as the UMG widget's. */
	FString AppliedDocumentPath;
};
