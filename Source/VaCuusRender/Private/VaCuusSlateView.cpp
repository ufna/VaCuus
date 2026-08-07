// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaCuusSlateView.h"

#include "VaCuusRmlDocumentHost.h"
#include "VaCuusSlateElement.h"

/**
 * The two definitions the opaque handles need, and the reason this file is one page long:
 * both types are complete HERE, so TSharedRef's reference controller captures a deleter
 * that knows how to destroy an FVaCuusSlateElement, and every caller across the module
 * boundary can hold, copy and drop the handle with only the forward declaration.
 */

namespace VaCuusSlateView
{
TSharedRef<FVaCuusSlateElement> MakeElement()
{
	return MakeShared<FVaCuusSlateElement>();
}

TUniquePtr<IVaCuusDocumentHost> MakeDocumentHost(const TSharedRef<FVaCuusSlateElement>& Element)
{
	// The element IS the frame sink -- FVaCuusSlateElement implements IVaCuusFrameSink,
	// which is what the host's constructor actually asks for. That the two are one object
	// on the screen path is exactly the kind of internal fact this façade exists to keep
	// internal: a caller says "host over this element" and never meets the sink interface.
	return MakeUnique<FVaCuusRmlDocumentHost>(Element);
}
}	 // namespace VaCuusSlateView
