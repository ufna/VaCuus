// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

/**
 * THE ONE PLACE quickjs.h IS INCLUDED FROM. Nothing in this module may include
 * "quickjs.h" directly; include this instead.
 *
 * WHY IT EXISTS -- the first Win64 build, which is the first time the vendored header met
 * MSVC. quickjs.h:856 is
 *
 *     static inline JSValue JS_ToBoolean(JSContext *ctx, JSValueConst val)
 *     { return JS_NewBool(ctx, JS_ToBool(ctx, val)); }
 *
 * and JS_ToBool returns int while JS_NewBool takes a bool, so MSVC raises C4800
 * ("implicit conversion to bool, possible information loss") in the HEADER, once per
 * including TU -- 20 of them. clang has no such warning, which is why Linux and macOS
 * never saw it.
 *
 * WHY NOT THE MECHANISM VaCuusRml USES. VaCuusRml.Build.cs hands its vendored headers to
 * UBT as a *system* include path, which becomes /external:W0 on MSVC and -isystem on
 * clang. That is not available here: ModuleRules.cs:1281 declares only
 * PublicSystemIncludePaths -- there is no private system variant -- and quickjs's include
 * path is deliberately PRIVATE (VaCuusJs.Build.cs), because patch #1 strips JS_* symbol
 * visibility and an unreachable header is what makes a wrong include fail at compile time
 * instead of at load time. Making it public to buy a warning flag would trade a real
 * invariant for a cosmetic one. So the suppression is scoped to this header instead,
 * which is narrower than /external:W0 anyway: it names one warning rather than silencing
 * the whole file.
 *
 * The push/pop pair is what keeps it from leaking into the includer's own code.
 */

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
// C4800: implicit conversion to bool. quickjs.h:856, JS_ToBoolean.
#pragma warning(disable : 4800)
#endif

#include "quickjs.h"

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif
