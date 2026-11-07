// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusMaterialBoxDecorator.h"

#include "VaCuusDefines.h"

#include "HAL/IConsoleManager.h"

#include <RmlUi/Core/CompiledFilterShader.h>
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Decorator.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/PropertyDictionary.h>
#include <RmlUi/Core/RenderBox.h>
#include <RmlUi/Core/RenderManager.h>

static TAutoConsoleVariable<int32> CVarVaCuusMaterialBox(
	TEXT("vacuus.MaterialBox"),
	1,
	TEXT("1 (default) = `decorator: material-box(...)` draws Slate's nine-slice grid. 0 = a plain 0..1 quad ")
	TEXT("(DrawAs=Image: corners stretch). Read when an element's decorator data is generated, so it is a ")
	TEXT("kill-switch, not a live toggle."));

namespace VaCuusMaterialBox
{
bool IsEnabled()
{
	return CVarVaCuusMaterialBox.GetValueOnAnyThread() != 0;
}

void BuildMesh(const FParams& Params, Rml::Vector2f Offset, Rml::Vector2f Size, float W, float H,
	Rml::ColourbPremultiplied Colour, Rml::Mesh& OutMesh)
{
	const float mT = Params.Margin[0], mR = Params.Margin[1], mB = Params.Margin[2], mL = Params.Margin[3];
	if (mT + mR + mB + mL <= 0.f)
	{
		Rml::MeshUtilities::GenerateQuad(OutMesh, Offset, Size, Colour, Rml::Vector2f(0.f), Rml::Vector2f(1.f));
		return;
	}

	// ElementBatcher.cpp:857-874: the cut sits at ImageSize * Margin px and its UV IS the margin;
	// cuts that would cross meet in the middle and keep their UVs, so the corners squish.
	float X[4] = {0.f, W * mL, Size.x - W * mR, Size.x};
	float Y[4] = {0.f, H * mT, Size.y - H * mB, Size.y};
	if (X[2] < X[1])
	{
		X[1] = X[2] = Size.x * 0.5f;
	}
	if (Y[2] < Y[1])
	{
		Y[1] = Y[2] = Size.y * 0.5f;
	}
	const float U[4] = {0.f, mL, 1.f - mR, 1.f};
	const float V[4] = {0.f, mT, 1.f - mB, 1.f};

	for (int32 i = 0; i < 4; ++i)
	{
		X[i] += Offset.x;
		Y[i] += Offset.y;
	}
	// Inner cuts on whole pixels, outer edges as laid out (DecoratorNinePatch.cpp:101-103).
	X[1] = Rml::Math::Round(X[1]);
	X[2] = Rml::Math::Round(X[2]);
	Y[1] = Rml::Math::Round(Y[1]);
	Y[2] = Rml::Math::Round(Y[2]);

	OutMesh.vertices.resize(16);
	for (int32 j = 0; j < 4; ++j)
	{
		for (int32 i = 0; i < 4; ++i)
		{
			Rml::Vertex& Vertex = OutMesh.vertices[j * 4 + i];
			Vertex.position = {X[i], Y[j]};
			Vertex.tex_coord = {U[i], V[j]};
			Vertex.colour = Colour;
		}
	}

	// DecoratorNinePatch.cpp:124-138; cell 4 is the centre, which Border leaves out.
	static const int32 TopLeft[9] = {0, 1, 2, 4, 5, 6, 8, 9, 10};
	OutMesh.indices.reserve(9 * 6);
	for (int32 Cell = 0; Cell < 9; ++Cell)
	{
		if (Params.bBorder && Cell == 4)
		{
			continue;
		}
		const int32 I = TopLeft[Cell];
		OutMesh.indices.insert(OutMesh.indices.end(), {I, I + 4, I + 1, I + 1, I + 4, I + 5});
	}
}

namespace
{
struct FElementData
{
	Rml::Geometry Geometry;
	Rml::CompiledShader Shader;
};

class FDecorator final : public Rml::Decorator
{
public:
	explicit FDecorator(FParams InParams)
		: Params(MoveTemp(InParams))
	{
	}

	virtual Rml::DecoratorDataHandle GenerateElementData(Rml::Element* Element, Rml::BoxArea PaintArea) const override
	{
		Rml::RenderManager* RenderManager = Element->GetRenderManager();
		if (!RenderManager)
		{
			return INVALID_DECORATORDATAHANDLE;
		}

		const Rml::RenderBox Box = Element->GetRenderBox(PaintArea);
		const Rml::Vector2f Offset = Box.GetFillOffset();
		const Rml::Vector2f Size = Box.GetFillSize();
		if (Size.x <= 0.f || Size.y <= 0.f)
		{
			return INVALID_DECORATORDATAHANDLE;
		}

		// shader()'s desc plus paint_offset: the recorder resolves the key exactly as for shader().
		Rml::CompiledShader Shader = RenderManager->CompileShader("shader",
			Rml::Dictionary{{"value", Rml::Variant(Params.Key)}, {"dimensions", Rml::Variant(Size)}, {"paint_offset", Rml::Variant(Offset)}});
		if (!Shader)
		{
			return INVALID_DECORATORDATAHANDLE;
		}

		const Rml::ComputedValues& Computed = Element->GetComputedValues();
		const Rml::ColourbPremultiplied Tint = Computed.image_color().ToPremultiplied(Computed.opacity());

		FParams Effective = Params;
		if (!IsEnabled())
		{
			FMemory::Memzero(Effective.Margin);
		}

		Rml::Mesh Mesh;
		BuildMesh(Effective, Offset, Size, Element->ResolveNumericValue(Params.ImageWidth, Size.x),
			Element->ResolveNumericValue(Params.ImageHeight, Size.y), Tint, Mesh);

		FElementData* Data = new FElementData{RenderManager->MakeGeometry(MoveTemp(Mesh)), MoveTemp(Shader)};
		return reinterpret_cast<Rml::DecoratorDataHandle>(Data);
	}

	virtual void ReleaseElementData(Rml::DecoratorDataHandle Handle) const override
	{
		delete reinterpret_cast<FElementData*>(Handle);
	}

	virtual void RenderElement(Rml::Element* Element, Rml::DecoratorDataHandle Handle) const override
	{
		FElementData* Data = reinterpret_cast<FElementData*>(Handle);
		Data->Geometry.Render(Element->GetAbsoluteOffset(Rml::BoxArea::Border), {}, Data->Shader);
	}

private:
	FParams Params;
};

class FInstancer final : public Rml::DecoratorInstancer
{
public:
	FInstancer()
	{
		KeyId = RegisterProperty("key", "").AddParser("string").GetId();
		ImageWidthId = RegisterProperty("image-width", "0px").AddParser("length").GetId();
		ImageHeightId = RegisterProperty("image-height", "0px").AddParser("length").GetId();
		MarginIds[0] = RegisterProperty("margin-top", "0").AddParser("number").GetId();
		MarginIds[1] = RegisterProperty("margin-right", "0").AddParser("number").GetId();
		MarginIds[2] = RegisterProperty("margin-bottom", "0").AddParser("number").GetId();
		MarginIds[3] = RegisterProperty("margin-left", "0").AddParser("number").GetId();
		DrawAsId = RegisterProperty("draw-as", "box").AddParser("keyword", "box, border").GetId();

		RegisterShorthand("image-size", "image-width, image-height", Rml::ShorthandType::Replicate);
		RegisterShorthand("margin", "margin-top, margin-right, margin-bottom, margin-left", Rml::ShorthandType::Box);
		RegisterShorthand("decorator", "key, image-size, margin, draw-as?", Rml::ShorthandType::RecursiveCommaSeparated);
	}

	virtual Rml::SharedPtr<Rml::Decorator> InstanceDecorator(const Rml::String& /*Name*/, const Rml::PropertyDictionary& Properties,
		const Rml::DecoratorInstancerInterface& /*Interface*/) override
	{
		FParams Params;
		const Rml::Property* Key = Properties.GetProperty(KeyId);
		const Rml::Property* Width = Properties.GetProperty(ImageWidthId);
		const Rml::Property* Height = Properties.GetProperty(ImageHeightId);
		if (!Key || !Width || !Height)
		{
			return nullptr;
		}
		Params.Key = Key->Get<Rml::String>();
		Params.ImageWidth = Width->GetNumericValue();
		Params.ImageHeight = Height->GetNumericValue();
		if (Params.Key.empty() || Params.ImageWidth.number <= 0.f || Params.ImageHeight.number <= 0.f)
		{
			UE_LOG(LogVaCuus, Warning, TEXT("material-box: needs a style key and a positive image-size; decorator ignored"));
			return nullptr;
		}

		bool bClamped = false;
		for (int32 Side = 0; Side < 4; ++Side)
		{
			const Rml::Property* Margin = Properties.GetProperty(MarginIds[Side]);
			const float Value = Margin ? Margin->Get<float>() : 0.f;
			Params.Margin[Side] = FMath::Clamp(Value, 0.f, 1.f);
			bClamped |= Params.Margin[Side] != Value;
		}
		if (bClamped)
		{
			UE_LOG(LogVaCuus, Warning, TEXT("material-box(%hs): a margin is outside 0..1, clamped"), Params.Key.c_str());
		}

		const Rml::Property* DrawAs = Properties.GetProperty(DrawAsId);
		Params.bBorder = DrawAs && DrawAs->Get<int>() == 1;

		return Rml::MakeShared<FDecorator>(MoveTemp(Params));
	}

private:
	Rml::PropertyId KeyId, ImageWidthId, ImageHeightId, DrawAsId;
	Rml::PropertyId MarginIds[4];
};
} // namespace

// Per boot: the instancer's property parsers belong to the RmlUi instance that built it.
static TUniquePtr<FInstancer> GInstancer;

void Register()
{
	GInstancer = MakeUnique<FInstancer>();
	Rml::Factory::RegisterDecoratorInstancer("material-box", GInstancer.Get());
}

void Unregister()
{
	GInstancer.Reset();
}
} // namespace VaCuusMaterialBox
