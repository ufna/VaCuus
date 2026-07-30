// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusModelShadow.h"

#include "VaCuusDefines.h"

#include "UObject/Class.h"

FVaCuusModelShadow::FVaCuusModelShadow(const UScriptStruct* InStruct)
{
	if (InStruct == nullptr)
	{
		UE_LOG(LogVaCuus, Error, TEXT("VaCuus model shadow: no struct type was given; the shadow is empty"));
		return;
	}

	Struct.Reset(InStruct);

	// ALIGNED, NOT PLAIN Malloc. UScriptStruct::InitializeStruct hard-asserts the
	// destination's alignment against the C++ type's own alignof before it constructs
	// (Class.cpp:3794-3798), so a 16-byte-aligned member such as an FVector4 or anything
	// carrying an FQuat would trip a checkf here rather than misbehave later.
	// GetStructureSize() is already Align(PropertiesSize, MinAlignment) (Class.h:796-799),
	// so size and alignment agree by construction.
	const int32 Size = InStruct->GetStructureSize();
	const int32 Alignment = InStruct->GetMinAlignment();

	Data = static_cast<uint8*>(FMemory::Malloc(Size, Alignment));

	// Memzero FIRST, then construct -- the same order UScriptStruct::InitializeStruct
	// itself uses (Class.cpp:3783 then :3798). It is not redundant: a struct's C++
	// constructor initialises its members but not its PADDING, and the differ that will
	// compare two of these buffers must never see padding that differs between them.
	// InitializeStruct does the memzero for the whole stride, so this is stated rather
	// than repeated.
	InStruct->InitializeStruct(Data);
}

FVaCuusModelShadow::~FVaCuusModelShadow()
{
	Reset();
}

FVaCuusModelShadow::FVaCuusModelShadow(FVaCuusModelShadow&& Other)
	: Struct(MoveTemp(Other.Struct))
	, Data(Other.Data)
{
	// The moved-from shadow must not destroy the buffer we just took. TStrongObjectPtr's
	// move leaves Other.Struct null already; this is the half the compiler will not do.
	Other.Data = nullptr;
}

FVaCuusModelShadow& FVaCuusModelShadow::operator=(FVaCuusModelShadow&& Other)
{
	if (this != &Other)
	{
		Reset();

		Struct = MoveTemp(Other.Struct);
		Data = Other.Data;
		Other.Data = nullptr;
	}

	return *this;
}

void FVaCuusModelShadow::Reset()
{
	if (Data != nullptr)
	{
		// ORDER: destroy through the type, then free. DestroyStruct runs the C++ destructor
		// when the type has one (Class.cpp:3946-3952) and otherwise walks DestructorLink
		// calling DestroyValue_InContainer per property (:3963-3973), which is what releases
		// every FString / FText / FSoftObjectPtr allocation in the buffer. Freeing without
		// it leaks all of them silently -- there is no reflection-level ownership anywhere
		// else to catch it.
		//
		// The null check on the type is not defensive noise: TStrongObjectPtr keeps a
		// UUserDefinedStruct alive, so it can only be null if this shadow was never
		// constructed with a type -- in which case Data is null too and we are not here.
		if (const UScriptStruct* ScriptStruct = Struct.Get())
		{
			ScriptStruct->DestroyStruct(Data);
		}
		else
		{
			UE_LOG(LogVaCuus, Error,
				TEXT("VaCuus model shadow: the struct type is gone before its buffer; every value in it is leaked"));
		}

		FMemory::Free(Data);
		Data = nullptr;
	}

	Struct.Reset();
}
