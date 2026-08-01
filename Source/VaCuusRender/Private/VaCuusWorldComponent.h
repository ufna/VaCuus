// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Components/MeshComponent.h"

#include "VaCuusWorldComponent.generated.h"

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
UCLASS(Blueprintable, ClassGroup = "UserInterface", meta = (BlueprintSpawnableComponent, DisplayName = "VaCuus World Panel"),
	hidecategories = (Object, Activation, "Components|Activation", Sockets, Base, Lighting, LOD, Mesh))
class UVaCuusWorldComponent : public UMeshComponent
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

	/** Re-sizes the view, render target, quad and collision. Degenerate sizes are refused with an Error. */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	void SetDrawSize(FIntPoint NewDrawSize);

	/** See bTwoSided. */
	UFUNCTION(BlueprintCallable, Category = "VaCuus")
	void SetTwoSided(bool bInTwoSided);

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

	/** Diagnostics/tests: the sink and its copy/skip counters. Null before registration. */
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

	/** The copy destination; non-sRGB PF_B8G8R8A8, extent == CurrentDrawSize. */
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
