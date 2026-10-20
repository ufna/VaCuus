/*
 * @vacuus/preact adapts stock Preact 11.0.1 through its public options hooks.
 * The E-P observations document the original Preact 10 integration; the stock
 * fixtures and AdapterContract now exercise Preact 11 against the same facade.
 *
 * Normalize event prop keys for RmlUi's event names, and className to the class
 * attribute for DOM vnodes. Preact 11 itself lowercases onClick, so that first
 * compatibility rule is now redundant for built-in events; keeping it preserves
 * the adapter's existing event-name contract. Component props stay untouched.
 *
 * Pin debounceRendering to the runtime's Promise job drain and effects to the
 * facade's requestAnimationFrame. An input callback's setState then commits in
 * the same UI frame, while effects run at the next frame's pump.
 *
 * The innerHTML alias routes dangerouslySetInnerHTML to innerRML. That reparses
 * RML, including {{...}} expressions: user text must use text children, whose
 * createTextNode/data paths bypass the binding scanner. Render into a dedicated
 * empty mount element; the stock fixtures separately check pre-existing DOM.
 * See Web/packages/preact-vacuus/README.md for the remaining facade deviations.
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
