/*
 * @vacuus/preact — stock preact 10.29.7 over the VaCuus DOM facade, adapted
 * through preact's own `options` hooks instead of a source fork.
 *
 * WHY OPTIONS AND NOT A FORK (the spec §2(j)/§3.1 deviation, deliberate): the
 * three divergences E-P observed (docs/research/m5-api-notes/ep-observations.md)
 * are all reachable from `options.vnode` / the two scheduling pins, which are
 * preact's PUBLIC extension points — the exact seam preact/compat itself uses.
 * A fork would re-own 4 KB of dist internals to change the same three behaviors
 * and would have to be re-forked on every preact upgrade; the adapter survives
 * upgrades as long as the options contract does, and the in-engine tests pin the
 * OBSERVED behavior either way. The spec said "patched preact"; this achieves
 * the identical observed behavior and the deviation is recorded here and in
 * ep-observations.md.
 *
 * THE THREE PATCHES:
 *
 * 1. on* PROP KEYS ARE LOWERCASED (E-P2). Stock preact lowercases an event name
 *    only when `'onclick' in dom` (src/diff/props.js:84-87); the facade wrappers
 *    have no on* properties, so `onClick` registered RmlUi type "Click" — which
 *    lowercase-typed RmlUi never fires (observed: the handler stayed silent).
 *    Lowercasing the KEY in options.vnode makes preact's own `.slice(2)` yield
 *    "click". Capture still works: preact strips the Capture suffix with a
 *    case-INsensitive regex (/(PointerCapture)$|Capture$/i, src/diff/props.js:81),
 *    so "onclickcapture" still parses as click+capture, and "pointercapture"
 *    event names still survive via the regex's first alternative.
 *
 * 2. className → class ON DOM VNODES (E-P2, the loud errata). Outside SVG stock
 *    preact emits setAttribute("className", …) — a dead attribute no RCSS
 *    selector matches; the class is silently lost (the rename exists only in the
 *    SVG branch, src/diff/props.js:113-118). Renamed here for every DOM vnode:
 *    RmlUi has no SVG namespace machinery at all (the facade's createElementNS
 *    ignores the namespace), and preact's SVG-branch rename never sees a
 *    "className" we already renamed, so a universal rename cannot double-apply.
 *    When both `class` and `className` are present, `class` wins and the
 *    duplicate is dropped. Component vnodes are left alone — their props are the
 *    component's contract, not the DOM's.
 *
 * 3. SCHEDULING PINNED TO THE FACADE (preact-contract.md §6). debounceRendering
 *    = Promise.resolve().then — preact's own default, pinned so a future preact
 *    can't change it: a setState commits in the SAME UI frame's job drain,
 *    before Context::Update lays it out. options.requestAnimationFrame = the
 *    facade rAF — effect flushing lands at the next frame's pump top
 *    deterministically instead of racing preact's setTimeout(35) fallback
 *    (hooks/src/index.js afterPaint).
 *
 * PLUS ONE FACADE EXTRA: an `innerHTML` accessor aliasing `innerRML`, installed
 * on the element prototype so `dangerouslySetInnerHTML` (which assigns
 * `dom.innerHTML` directly, src/diff/index.js:531) reaches RmlUi instead of
 * dying as an inert expando. THE BRACE HAZARD RIDES THIS DOOR, documented:
 * innerRML is an RML re-parse, so `{{…}}` inside the assigned string IS a data
 * expression to RmlUi's scanner (Factory.cpp:343-392) — never feed
 * dangerouslySetInnerHTML user-supplied data; text children take the facade's
 * scanner-bypassed createTextNode path and render braces literally.
 *
 * DOCUMENTED FACADE DEVIATIONS a component author inherits (preact-contract.md):
 *  - e.preventDefault() maps to StopPropagation — it also stops bubbling
 *    (RmlUi has no separate default-action veto, VaCuusJsEvents.cpp:321-330).
 *  - onFocusIn/onFocusOut register "focusin"/"focusout", which RmlUi never
 *    fires; use onFocus/onBlur.
 *  - Mounting into `document` / `document.body` cannot work (E-P1b): render into
 *    a dedicated empty mount element, which the CLI template provides.
 */

import { options } from 'preact';

export * from 'preact';
export * from 'preact/hooks';

/** Feature-test marker so a bundle can assert it built against the adapter. */
export const VACUUS_ADAPTER = 1;

let innerHTMLInstalled = false;

function installInnerHTMLAlias() {
	// `document` is a global that exists from context birth and is null until a
	// document binds (VaCuusJsViewContext.cpp InstallGlobals); via the CLI
	// template's <script src> path it is always bound before the bundle runs, but
	// the retry from onVnode below keeps a hand-loaded bundle honest too.
	if (innerHTMLInstalled || typeof document === 'undefined' || document === null) return;
	const proto = Object.getPrototypeOf(document.createElement('div'));
	if (!Object.getOwnPropertyDescriptor(proto, 'innerHTML')) {
		const rml = Object.getOwnPropertyDescriptor(proto, 'innerRML');
		if (!rml) return; // not the VaCuus facade; leave the host DOM alone
		Object.defineProperty(proto, 'innerHTML', {
			configurable: true,
			get() { return rml.get.call(this); },
			set(v) { rml.set.call(this, v); },
		});
	}
	innerHTMLInstalled = true;
}

const prevVnode = options.vnode;

options.vnode = (vnode) => {
	installInnerHTMLAlias();

	const props = vnode.props;
	// DOM vnodes only: a string type is an element; functions/classes keep their
	// props verbatim (patch 2's last sentence).
	if (props != null && typeof vnode.type === 'string') {
		let rename = null;
		for (const name in props) {
			if (name === 'className') {
				(rename = rename || {})[name] = 'class' in props ? null : 'class';
			} else if (
				name[0] === 'o' && name[1] === 'n' && name !== (name.toLowerCase())
			) {
				(rename = rename || {})[name] = name.toLowerCase();
			}
		}
		if (rename) {
			for (const from in rename) {
				const to = rename[from];
				if (to !== null && !(to in props)) props[to] = props[from];
				delete props[from];
			}
		}
	}

	if (prevVnode) prevVnode(vnode);
};

// Patch 3 — the scheduling pins (see the header). Assigned unconditionally at
// import: this module IS the app's preact in a @vacuus/preact bundle, so there
// is no earlier assignment to preserve.
options.debounceRendering = (cb) => Promise.resolve().then(cb);
options.requestAnimationFrame = (cb) => requestAnimationFrame(cb);
