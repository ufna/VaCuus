/*
 * `vacuus lint` — the CLI's authoring-time checks for the RCSS/facade gotchas
 * the architecture spec's risk table names (2026-07-29-vacuus-architecture-design.md
 * §13). Simple, honest heuristics: each rule states exactly what it matches, each
 * ships one known-bad fixture that fires and one clean fixture that stays silent
 * (fixtures/lint/, exercised by Web/smoke.mjs), and a false positive is
 * suppressible with the marker "vacuus-lint-allow <rule>" placed in a comment on
 * the flagged line (any comment style the file type supports).
 *
 * Findings: { rule, file, line, message }.
 */

import { readFileSync } from 'node:fs';

const LAYOUT_PROPS =
	'width|height|top|left|right|bottom|margin(?:Top|Right|Bottom|Left)?|padding(?:Top|Right|Bottom|Left)?|fontSize|font-size';

/** Line number (1-based) of a character offset. */
function lineOf(text, offset) {
	let line = 1;
	for (let i = 0; i < offset && i < text.length; ++i) if (text[i] === '\n') ++line;
	return line;
}

function allowed(text, offset, rule) {
	const lineStart = text.lastIndexOf('\n', offset) + 1;
	const lineEnd = text.indexOf('\n', offset);
	const line = text.slice(lineStart, lineEnd === -1 ? text.length : lineEnd);
	return line.includes(`vacuus-lint-allow ${rule}`);
}

/**
 * RULE bare-attribute-selector (.rcss): a compound selector whose SUBJECT is an
 * attribute selector — `[disabled] { … }` as opposed to `button[disabled]`.
 * Two real costs behind it: an unanchored subject is universal (matched against
 * every element on every style resolution), and — the facade-specific trap —
 * classList writes go through SetClass and NEVER write the class ATTRIBUTE
 * (VaCuusJsDom.cpp classList; Element.cpp:258-276), so a `[class…]` selector
 * silently disagrees with classList-driven state.
 */
function lintBareAttributeSelector(file, text, findings) {
	// Selector text = whatever sits between a block boundary (} ; { or file
	// start) and an opening {. @-rules are skipped whole.
	const selectorRe = /(?:^|[}{;])([^{}@;]*)\{/g;
	for (const m of text.matchAll(selectorRe)) {
		const selectors = m[1];
		const base = m.index + (m[0].length - m[1].length - 1);
		for (const part of selectors.split(',')) {
			// Compounds are separated by whitespace or combinators.
			for (const compound of part.split(/[\s>+~]+/)) {
				if (compound.startsWith('[') && !allowed(text, base, 'bare-attribute-selector')) {
					findings.push({
						rule: 'bare-attribute-selector',
						file,
						line: lineOf(text, base),
						message:
							`'${compound}' has no element/class/id anchor: an attribute-only subject is matched against every element ` +
							`on every style resolution. Anchor it (e.g. div${compound}) — and remember the facade's classList never ` +
							`writes the class ATTRIBUTE, so [class…] selectors lie under classList-driven state.`,
					});
				}
			}
		}
	}
}

/**
 * RULE box-shadow-animation (.rcss): box-shadow named in a `transition:` value
 * or keyed inside `@keyframes`. RmlUi refuses the key at animation start —
 * "Box shadows do not support animations or transitions", LT_WARNING,
 * ElementAnimation.cpp:640-648 — so the tween silently does nothing on screen;
 * this catches it at authoring time instead of in a runtime log.
 */
function lintBoxShadowAnimation(file, text, findings) {
	for (const m of text.matchAll(/transition\s*:[^;{}]*box-shadow/g)) {
		if (!allowed(text, m.index, 'box-shadow-animation')) {
			findings.push({
				rule: 'box-shadow-animation',
				file,
				line: lineOf(text, m.index),
				message:
					'box-shadow in a transition: RmlUi refuses it at animation start (ElementAnimation.cpp:640-648) and the ' +
					'tween does nothing on screen. Animate opacity/transform on a wrapper, or swap decorators.',
			});
		}
	}
	for (const km of text.matchAll(/@keyframes\b[^{]*\{/g)) {
		// The keyframes block body, by brace counting from its opening brace.
		let depth = 1;
		const start = km.index + km[0].length;
		let end = start;
		while (end < text.length && depth > 0) {
			if (text[end] === '{') ++depth;
			else if (text[end] === '}') --depth;
			++end;
		}
		const body = text.slice(start, end);
		const inner = body.match(/box-shadow\s*:/);
		if (inner && !allowed(text, start + inner.index, 'box-shadow-animation')) {
			findings.push({
				rule: 'box-shadow-animation',
				file,
				line: lineOf(text, start + inner.index),
				message:
					'box-shadow keyed inside @keyframes: RmlUi refuses it at animation start (ElementAnimation.cpp:640-648); ' +
					'the key silently drops.',
			});
		}
	}
}

/**
 * RULE layout-thrash (.tsx/.ts/.jsx/.js): a layout-driving style write inside a
 * requestAnimationFrame or setInterval callback. Every such write dirties layout
 * for that UI frame — a per-frame one re-layouts the document continuously.
 * Prefer transform (render-level, no layout), or gate the write on change (the
 * m4_hud_logic.js change-gate pattern). Heuristic scope: the callback is taken
 * to be the call's argument list up to the matching close paren.
 */
function lintLayoutThrash(file, text, findings) {
	const writeRe = new RegExp(
		`\\.style\\.(?:${LAYOUT_PROPS})\\s*=|\\.style\\.setProperty\\(\\s*['"](?:${LAYOUT_PROPS})['"]`, 'g');
	for (const call of text.matchAll(/\b(?:requestAnimationFrame|setInterval)\s*\(/g)) {
		let depth = 1;
		const start = call.index + call[0].length;
		let end = start;
		while (end < text.length && depth > 0) {
			if (text[end] === '(') ++depth;
			else if (text[end] === ')') --depth;
			++end;
		}
		const body = text.slice(start, end);
		writeRe.lastIndex = 0;
		const w = writeRe.exec(body);
		if (w && !allowed(text, start + w.index, 'layout-thrash')) {
			findings.push({
				rule: 'layout-thrash',
				file,
				line: lineOf(text, start + w.index),
				message:
					`layout-driving style write ('${w[0].trim()}') inside a per-frame callback: every write dirties layout for ` +
					'that UI frame. Prefer transform, or gate the write on change (m4_hud_logic.js does).',
			});
		}
	}
}

/** Lints one file by extension; returns findings. */
export function lintFile(file) {
	const text = readFileSync(file, 'utf8');
	const findings = [];
	if (file.endsWith('.rcss') || file.endsWith('.css')) {
		lintBareAttributeSelector(file, text, findings);
		lintBoxShadowAnimation(file, text, findings);
	} else if (/\.(tsx|ts|jsx|js|mjs)$/.test(file)) {
		lintLayoutThrash(file, text, findings);
	}
	return findings;
}

export function runLint(files) {
	let total = 0;
	for (const file of files) {
		for (const f of lintFile(file)) {
			++total;
			console.error(`${f.file}:${f.line}: [${f.rule}] ${f.message}`);
		}
	}
	if (total === 0) console.log(`lint clean (${files.length} file(s))`);
	return total === 0 ? 0 : 1;
}
