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

namespace
{
// GenerateBackground owns RmlUi's radius clamping and elliptical inner corners
// (MeshUtilities.cpp:118-139, GeometryBackgroundBorder.cpp:11-93). Intersecting each
// sliced triangle with that convex outline preserves its UV interpolation and avoids
// a separate stencil operation that could disturb an enclosing overflow clip.
void ClipToPaintBox(const Rml::RenderBox* PaintBox, Rml::ColourbPremultiplied Colour, Rml::Mesh& Mesh)
{
	if (!PaintBox)
	{
		return;
	}
	Rml::Mesh Outline;
	Rml::MeshUtilities::GenerateBackground(Outline, *PaintBox, Colour);
	if (Outline.vertices.size() <= 4)
	{
		return;
	}
	auto Cross = [](Rml::Vector2f A, Rml::Vector2f B, Rml::Vector2f C)
	{
		return (B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x);
	};
	float Area = 0.f;
	for (size_t I = 0; I < Outline.vertices.size(); ++I)
	{
		const Rml::Vector2f A = Outline.vertices[I].position;
		const Rml::Vector2f B = Outline.vertices[(I + 1) % Outline.vertices.size()].position;
		Area += A.x * B.y - A.y * B.x;
	}
	const float Winding = Area >= 0.f ? 1.f : -1.f;
	Rml::Mesh Clipped;
	TArray<Rml::Vertex, TInlineAllocator<32>> Polygon, Next;
	for (size_t I = 0; I + 2 < Mesh.indices.size(); I += 3)
	{
		Polygon.Reset();
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			Polygon.Add(Mesh.vertices[Mesh.indices[I + Corner]]);
		}
		for (size_t Edge = 0; Edge < Outline.vertices.size() && !Polygon.IsEmpty(); ++Edge)
		{
			const Rml::Vector2f A = Outline.vertices[Edge].position;
			const Rml::Vector2f B = Outline.vertices[(Edge + 1) % Outline.vertices.size()].position;
			Next.Reset();
			Rml::Vertex Previous = Polygon.Last();
			float PreviousDistance = Winding * Cross(A, B, Previous.position);
			for (const Rml::Vertex& Current : Polygon)
			{
				const float Distance = Winding * Cross(A, B, Current.position);
				if ((Distance >= 0.f) != (PreviousDistance >= 0.f))
				{
					const float T = PreviousDistance / (PreviousDistance - Distance);
					Rml::Vertex Intersection;
					Intersection.position = Previous.position + (Current.position - Previous.position) * T;
					Intersection.tex_coord = Previous.tex_coord + (Current.tex_coord - Previous.tex_coord) * T;
					Intersection.colour = Colour;
					Next.Add(Intersection);
				}
				if (Distance >= 0.f)
				{
					Next.Add(Current);
				}
				Previous = Current;
				PreviousDistance = Distance;
			}
			Swap(Polygon, Next);
		}
		if (Polygon.Num() < 3)
		{
			continue;
		}
		const int32 First = int32(Clipped.vertices.size());
		for (const Rml::Vertex& Vertex : Polygon)
		{
			Clipped.vertices.push_back(Vertex);
		}
		for (int32 Corner = 1; Corner + 1 < Polygon.Num(); ++Corner)
		{
			if (FMath::Abs(Cross(Polygon[0].position, Polygon[Corner].position, Polygon[Corner + 1].position)) > UE_SMALL_NUMBER)
			{
				Clipped.indices.insert(Clipped.indices.end(), {First, First + Corner, First + Corner + 1});
			}
		}
	}
	Mesh = MoveTemp(Clipped);
}
} // namespace

void BuildMesh(const FParams& Params, Rml::Vector2f Offset, Rml::Vector2f Size, float W, float H,
	Rml::ColourbPremultiplied Colour, Rml::Mesh& OutMesh, const Rml::RenderBox* PaintBox)
{
	OutMesh.vertices.clear();
	OutMesh.indices.clear();
	const float mT = Params.Margin[0], mR = Params.Margin[1], mB = Params.Margin[2], mL = Params.Margin[3];
	if (mT + mR + mB + mL <= 0.f)
	{
		if (PaintBox)
		{
			Rml::MeshUtilities::GenerateBackground(OutMesh, *PaintBox, Colour);
			for (Rml::Vertex& Vertex : OutMesh.vertices)
			{
				Vertex.tex_coord = (Vertex.position - Offset) / Size;
			}
		}
		else
		{
			Rml::MeshUtilities::GenerateQuad(OutMesh, Offset, Size, Colour, Rml::Vector2f(0.f), Rml::Vector2f(1.f));
		}
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
	X[1] = FMath::Clamp(Rml::Math::Round(X[1]), X[0], X[3]);
	X[2] = FMath::Clamp(Rml::Math::Round(X[2]), X[1], X[3]);
	Y[1] = FMath::Clamp(Rml::Math::Round(Y[1]), Y[0], Y[3]);
	Y[2] = FMath::Clamp(Rml::Math::Round(Y[2]), Y[1], Y[3]);

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

	// Slate Border repeats the middle strip, with at least one full strip when the
	// element is small (ElementBatcher.cpp:2888-2916). Separate tile quads retain UV
	// discontinuities before material vertex evaluation, including Customized UVs.
	const float RepeatX = W * (U[2] - U[1]);
	const float RepeatY = H * (V[2] - V[1]);
	const double TilesX = Params.bBorder && RepeatX > UE_SMALL_NUMBER ? FMath::Max(1.0, double(X[2] - X[1]) / RepeatX) : 1.0;
	const double TilesY = Params.bBorder && RepeatY > UE_SMALL_NUMBER ? FMath::Max(1.0, double(Y[2] - Y[1]) / RepeatY) : 1.0;
	// An arbitrarily small image-size must not turn a CSS declaration into an unbounded
	// UI-thread allocation. Refuse before converting the counts or appending tile geometry.
	if (!FMath::IsFinite(TilesX) || !FMath::IsFinite(TilesY) || 2.0 * (FMath::CeilToDouble(TilesX) + FMath::CeilToDouble(TilesY)) > 4096.0)
	{
		OutMesh.vertices.clear();
		UE_LOG(LogVaCuus, Warning, TEXT("material-box: border exceeds 4096 edge tiles; decorator ignored"));
		return;
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
		const int32 Row = Cell / 3, Column = Cell % 3;
		const bool bHorizontal = Params.bBorder && Column == 1 && TilesX > 1.0;
		const bool bVertical = Params.bBorder && Row == 1 && TilesY > 1.0;
		if (bHorizontal || bVertical)
		{
			const int32 Count = FMath::CeilToInt(bHorizontal ? TilesX : TilesY);
			for (int32 Tile = 0; Tile < Count; ++Tile)
			{
				const float Start = bHorizontal ? X[1] : Y[1];
				const float End = bHorizontal ? X[2] : Y[2];
				const float Period = bHorizontal ? RepeatX : RepeatY;
				const float P0 = Start + float(Tile) * Period;
				const float P1 = FMath::Min(P0 + Period, End);
				const float Fraction = (P1 - P0) / Period;
				const Rml::Vector2f Origin(bHorizontal ? P0 : X[Column], bVertical ? P0 : Y[Row]);
				const Rml::Vector2f Extent(bHorizontal ? P1 - P0 : X[Column + 1] - X[Column],
					bVertical ? P1 - P0 : Y[Row + 1] - Y[Row]);
				const Rml::Vector2f UV0(U[Column], V[Row]);
				const Rml::Vector2f UV1(bHorizontal ? U[1] + Fraction * (U[2] - U[1]) : U[Column + 1],
					bVertical ? V[1] + Fraction * (V[2] - V[1]) : V[Row + 1]);
				Rml::MeshUtilities::GenerateQuad(OutMesh, Origin, Extent, Colour, UV0, UV1);
			}
		}
		else
		{
			const int32 I = TopLeft[Cell];
			OutMesh.indices.insert(OutMesh.indices.end(), {I, I + 4, I + 1, I + 1, I + 4, I + 5});
		}
	}
	ClipToPaintBox(PaintBox, Colour, OutMesh);
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
			Element->ResolveNumericValue(Params.ImageHeight, Size.y), Tint, Mesh, &Box);
		if (Mesh.indices.empty())
		{
			return INVALID_DECORATORDATAHANDLE;
		}

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
