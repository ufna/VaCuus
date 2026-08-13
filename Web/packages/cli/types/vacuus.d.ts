/*
 * vacuus.d.ts — the HAND-WRITTEN typed surface of the VaCuus JS facade
 * (the M5 research decision: hand-written over generated, because the facade is
 * ~50 members whose semantics need prose, not a mechanical mirror of thunks).
 *
 * THIS FILE IS LOAD-BEARING FOR A TEST. `vacuus manifest` extracts every name
 * declared here into Content/DevUI/Tests/vacuus-api-manifest.json (committed),
 * and the in-engine conformance test (VaCuus.Js.Preact.Conformance) walks the
 * REAL prototypes and globals against that manifest BOTH WAYS: a name typed here
 * but absent in the engine fails, and a name present in the engine but not typed
 * here fails. Editing this file without re-running `vacuus manifest` fails the
 * node-side staleness check (`vacuus manifest --check`, run by Web/smoke.mjs).
 *
 * EXTRACTION CONTRACT (lib/manifest.mjs): interface names map to manifest
 * groups — VaCuusElement -> element, VaCuusDocument (own members) -> document,
 * VaCuusEvent -> eventPrototype (methods) / eventInstance (required props) /
 * eventAliases (OPTIONAL props — excluded from the walk: they exist per dispatch
 * only when the RmlUi event carried the parameter, VaCuusJsEvents.cpp:258-284),
 * VaCuusClassList -> classList, VaCuusHost -> vacuus, VaCuusConsole -> console,
 * `declare var|function` statements -> globals. VaCuusStyle is deliberately NOT
 * walked: `style` mints a fresh JS Proxy per access whose own-key enumeration is
 * trap-defined, not prototype truth.
 */

/**
 * One RmlUi element (or text node, or the document — nodeType discriminates).
 * Wrappers are identity-cached: two lookups of the same live element are `===`.
 * THE NEVER-THROW CONTRACT: a dead wrapper (its element was destroyed — an
 * innerRML write on an ancestor, C++ tree surgery, document close) answers null/
 * undefined/false from every member instead of throwing; tree ops on it no-op.
 */
interface VaCuusElement {
	/** Returns the appended child, or null (cycle, cross-context, dead handle). */
	appendChild(child: VaCuusElement): VaCuusElement | null;
	/** Null/dead/absent ref appends (DOM semantics for null; a non-child ref appends where DOM throws). */
	insertBefore(child: VaCuusElement, ref: VaCuusElement | null): VaCuusElement | null;
	/** The removed child stays alive detached, identity preserved. */
	removeChild(child: VaCuusElement): VaCuusElement | null;
	/** Destroys this element and its subtree; every wrapper into it goes dead. */
	remove(): void;
	/** Deviation from DOM: self never matches. */
	querySelector(selector: string): VaCuusElement | null;
	querySelectorAll(selector: string): VaCuusElement[];
	/** Deviation from DOM: starts at the parent, so self never matches. */
	closest(selector: string): VaCuusElement | null;
	getAttribute(name: string): string | null;
	/** Synchronous: `setAttribute("class", …)` restyles immediately. */
	setAttribute(name: string, value: string | number | boolean): void;
	removeAttribute(name: string): void;
	id: string;
	/** LOWERCASE (RmlUi's GetTagName), i.e. DOM localName semantics — not DOM's uppercase tagName. */
	readonly tagName: string;
	/** RmlUi markup in/out. SET DESTROYS ALL CHILDREN synchronously — their wrappers go dead. */
	innerRML: string;
	readonly parentNode: VaCuusElement | null;
	/** DOM children only — #text children filtered out (that IS the DOM contract for children). */
	readonly children: VaCuusElement[];
	/** Unfiltered: #text children included. Fresh snapshot array per read. */
	readonly childNodes: VaCuusElement[];
	readonly firstChild: VaCuusElement | null;
	readonly lastChild: VaCuusElement | null;
	readonly nextSibling: VaCuusElement | null;
	readonly previousSibling: VaCuusElement | null;
	/** 1 element, 3 text, 9 document. */
	readonly nodeType: number;
	/** Alias of tagName (which already carries localName semantics). */
	readonly localName: string;
	/** Fresh {name, value} snapshot array per read (preact's adoption path walks it). */
	readonly attributes: { name: string; value: string }[];
	/**
	 * Text nodes only: the text, set through ElementText::SetText — NEVER an RML
	 * parse, so `{{…}}` in user data renders literally (the brace-injection
	 * bypass). Reads null on non-text elements; writes no-op there.
	 */
	nodeValue: string | null;
	/** Alias of nodeValue — preact's text-update write (`dom.data = value`). */
	data: string | null;
	/**
	 * Fresh object per access (`el.classList !== el.classList`). Backed by
	 * SetClass/IsClassSet, NEVER the class attribute: getAttribute("class") does
	 * not see classList writes. Under preact, class is preact's — drive it
	 * through props, not classList, on preact-owned nodes.
	 */
	readonly classList: VaCuusClassList;
	/** Fresh Proxy per access; camelCase maps to kebab-case; see VaCuusStyle. */
	readonly style: VaCuusStyle;
	/** DOM duplicate rule; options object honored for `capture` only. Non-function listener throws TypeError. */
	addEventListener(type: string, listener: (ev: VaCuusEvent) => void, options?: boolean | { capture?: boolean }): void;
	removeEventListener(type: string, listener: (ev: VaCuusEvent) => void, options?: boolean | { capture?: boolean }): void;
	/** params values cross as bool/number/string only. Returns DOM's "false if canceled". */
	dispatchEvent(type: string, params?: Record<string, boolean | number | string>): boolean;
}

/**
 * The document wrapper — RmlUi's ElementDocument IS the body element, so this
 * interface EXTENDS the element surface and `document.body === document`.
 */
interface VaCuusDocument extends VaCuusElement {
	/** Tag is lowercased. `createElement("#text")` mints a text node (prefer createTextNode). */
	createElement(tag: string): VaCuusElement | null;
	/** Preact's ONLY element-creation call (E-P1); the namespace is ignored — RmlUi has none. */
	createElementNS(namespace: string | null, tag: string): VaCuusElement | null;
	/** Verbatim text, scanner-bypassed (see nodeValue). Numbers coerce to string. */
	createTextNode(text: string | number): VaCuusElement | null;
	getElementById(id: string): VaCuusElement | null;
	/** `document.body === document` — feature-test with nodeType 9, not identity. */
	readonly body: VaCuusDocument;
}

/**
 * The event object a listener receives — a FRESH object per invocation (no
 * identity across listeners of one dispatch; stashing one past the dispatch
 * leaves it dead-not-dangling). Inside a handler `this` is the currentTarget.
 * The OPTIONAL members exist only when the RmlUi event carried the matching
 * parameter (mouse events carry mouse_x, key events key_identifier, …).
 */
interface VaCuusEvent {
	/** DEVIATION: maps to StopPropagation — RmlUi has no separate default-action veto, so this also stops bubbling. */
	preventDefault(): void;
	stopPropagation(): void;
	stopImmediatePropagation(): void;
	readonly type: string;
	/** Identity-cached: `ev.target === document.getElementById(…)` holds. */
	readonly target: VaCuusElement | null;
	readonly currentTarget: VaCuusElement | null;
	/** DOM numbering: 1 capture, 2 at-target, 3 bubble. */
	readonly eventPhase: number;
	/** Every convertible RmlUi parameter, verbatim keys. */
	readonly params: Record<string, boolean | number | string>;
	readonly mouseX?: number;
	readonly mouseY?: number;
	readonly button?: number;
	readonly keyIdentifier?: number;
	readonly text?: string;
	readonly ctrlKey?: boolean;
	readonly shiftKey?: boolean;
	readonly altKey?: boolean;
	readonly metaKey?: boolean;
}

/** One token per call — no variadic add/remove (DOM allows several; the facade does not). */
interface VaCuusClassList {
	add(token: string): void;
	remove(token: string): void;
	toggle(token: string): boolean;
	contains(token: string): boolean;
}

/**
 * The style proxy (fresh per `.style` access). Property names map camelCase ->
 * kebab-case on get, set AND delete (`--custom` keys pass verbatim); values are
 * RmlUi property strings. BARE NUMBERS ARE REFUSED by RmlUi's parser with its
 * own warning — the facade appends no unit because preact appends `px` itself
 * (props.js:23); hand-written JS must write "100px", not 100.
 * NOT part of the conformance walk (see the header).
 */
interface VaCuusStyle {
	setProperty(name: string, value: string): boolean;
	removeProperty(name: string): void;
	[property: string]: any;
}

/** console.* — each argument stringified, joined by spaces, to LogVaCuusJS. */
interface VaCuusConsole {
	log(...args: any[]): void;
	info(...args: any[]): void;
	warn(...args: any[]): void;
	error(...args: any[]): void;
}

/** The `vacuus` host object (M4 spec 3.11 + the M5 localization hook). */
interface VaCuusHost {
	/**
	 * Fire-and-forget to the game thread (UVaCuusView::OnJsEvent). Payload: own
	 * enumerable string properties, flat; bool/number/string values cross,
	 * everything else is SKIPPED (not stringified). Returns false when dropped
	 * (queue full / no UI thread) — backpressure is observable where it happens.
	 */
	emit(name: string, payload?: Record<string, boolean | number | string>): boolean;
	/**
	 * Mints a FRESH {name, get} per call (`vacuus.model('hud') !==
	 * vacuus.model('hud')`); existence does not validate the name. get(path)
	 * reads the UI shadow ("Health", "Killfeed[0].Killer", "Rows.size"); an
	 * unbound name/path reads null with one Warning per (model, path).
	 */
	model(name: string): { readonly name: string; get(path: string): boolean | number | string | null };
	/** Process-wide UI-frame figures from the always-on last-sample store; zeros until the scopes have run. */
	stats(): { updateMs: number; renderMs: number; fps: number };
	/**
	 * Resident UI textures, process-wide (VaCuus-dqs.1). SEPARATE FROM stats() on purpose:
	 * that one is per-frame wall clock, this only moves when a document loads or drops an
	 * image, and a per-frame reader should not mistake one for the other.
	 *
	 * `bytes` IS THE LOGICAL FOOTPRINT — each texture's extent times its format's block bytes.
	 * The RHI may pad row pitch and nothing on this side can see that, so this is what the
	 * payloads occupy and NOT a VRAM figure.
	 *
	 * A PUBLISHED SNAPSHOT, so it can be one frame stale: the truth lives in the render
	 * thread's texture maps and reading them synchronously would stall the UI thread on the
	 * renderer once per call. Textures are shared by SOURCE, so N elements with one `src` count
	 * once — and RmlUi never evicts a file texture on its own, so a number that only grows is
	 * the documented behaviour rather than a leak.
	 */
	textureStats(): { count: number; bytes: number };
	/**
	 * Drop this view's cached copy of one file texture, keyed by the VFS source an `<img src>`
	 * was written with (VaCuus-dqs.2). Answers whether anything was actually cached under it —
	 * a mistyped source is the likeliest bug here, and false is how you find out.
	 *
	 * THE REASON THIS EXISTS: RmlUi's texture cache has NO EVICTION. Closing a document does
	 * not drop its images, so a catalogue screen holds every icon it has ever shown for the
	 * life of the UI thread. `vacuus.onUnload` is the natural place to call this.
	 *
	 * SAFE ON AN IMAGE STILL ON SCREEN — it reloads on next use, because elements hold an
	 * index rather than a handle. Releasing too eagerly costs a decode (a hitch), never a blank
	 * element. Per view by construction; a script cannot reach another view's cache.
	 *
	 * Throws TypeError on a non-string, and on '' — use releaseTextures() to mean all of them.
	 */
	releaseTexture(source: string): boolean;
	/** Every cached file texture in THIS view. See releaseTexture. */
	releaseTextures(): void;
	/** Getter, computed per read: width/height track the context through resizes. 0x0 before a document binds. */
	readonly view: { id: number; width: number; height: number };
	/**
	 * Assign a function; invoked at document close/replace/view removal, before
	 * the tree dies — the place to serialize transient state via emit. Null until
	 * assigned (feature-testable, like `document`).
	 */
	onUnload: (() => void) | null;
	/**
	 * Assign a function; invoked after the game publishes a new translation table, once per
	 * view. `tag` is whatever the pusher labelled it with ("ru", a culture name, anything) —
	 * the plugin never interprets it. Null until assigned, like `onUnload`.
	 *
	 * ORDERING IS GUARANTEED: the new table is already installed when this runs, so
	 * `vacuus.translate` inside the handler answers in the NEW language.
	 *
	 * You need this only for strings JS builds itself. Markup written `{{ t.key }}` inside a
	 * data model re-translates on its own, with no reload and no callback.
	 */
	onLanguageChanged: ((tag: string) => void) | null;
	/** console.log under the host name — the same thunk, so the two cannot drift. */
	log(...args: any[]): void;
	/**
	 * THE LOCALIZATION HOOK (M5 spec §2(l)). Synchronous lookup in the last
	 * translation table the game published (UVaCuusSubsystem::SetTranslationTable
	 * — publish-by-replacement snapshots; there is no per-call game callback,
	 * because this answers DURING JS execution on the UI thread). Identity on any
	 * miss; no table ever published logs one Verbose per context. Then `{name}`
	 * tokens are replaced from params (bool/number/string; integral numbers print
	 * without a decimal tail). Single braces render literally in RmlUi — only
	 * `{{` is a data expression — so an unsubstituted token is visible, not live.
	 */
	translate(key: string, params?: Record<string, boolean | number | string>): string;
}

/** NULL until a document is ready (feature-test `document === null`), then the document wrapper. */
declare var document: VaCuusDocument | null;
declare var console: VaCuusConsole;
declare var vacuus: VaCuusHost;

/** No extra-argument forwarding (browser setTimeout(fn, ms, a, b) drops a, b here). Ids are per-context, from 1. */
declare function setTimeout(callback: () => void, delayMs?: number): number;
declare function setInterval(callback: () => void, delayMs?: number): number;
/** One operation under both names; unknown ids (including 0) are no-ops. */
declare function clearTimeout(id: number): void;
declare function clearInterval(id: number): void;
/** Fires at the NEXT UI frame's pump top with the frame clock in MILLISECONDS (browser convention). Handles are their own namespace — cancelAnimationFrame(timerId) cancels nothing. */
declare function requestAnimationFrame(callback: (timestampMs: number) => void): number;
declare function cancelAnimationFrame(handle: number): void;
