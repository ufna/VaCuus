/*
 * The node-side smoke (spec §3.1, plan 8.2), controller-run: `node Web/smoke.mjs`
 * (after `npm install` in Web/). Asserts, in order:
 *
 *  1. LINT FIXTURES BOTH WAYS — each rule's known-bad fixture fires that exact
 *     rule, each clean fixture stays silent (an invariant with no observable
 *     rots — the linter's observable is these fixtures).
 *  2. MANIFEST PIPELINE — the committed vacuus-api-manifest.json matches the
 *     .d.ts (freshness), AND a deliberately-doctored manifest is DETECTED stale
 *     (the check's own restore-the-bug half).
 *  3. CREATE/BUILD ROUNDTRIP — `vacuus create` scaffolds, `vacuus build`
 *     bundles into a scratch DevUI root, provenance lands beside the bundle,
 *     the scaffolded RCSS lints clean.
 *  4. SYMBOLICATE — the roundtrip bundle's inline map resolves a generated
 *     position back into the TSX source.
 *
 * Exit 0 all-green; 1 with FAIL lines otherwise.
 */

import { readFileSync, writeFileSync, rmSync, existsSync, mkdtempSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { lintFile } from './packages/cli/lib/lint.mjs';
import { extractManifest, manifestJson, runManifest } from './packages/cli/lib/manifest.mjs';
import { runCreate } from './packages/cli/lib/create.mjs';
import { runBuild } from './packages/cli/lib/build.mjs';
import { loadInlineMap, mapPosition, symbolicateStack } from './packages/cli/lib/symbolicate.mjs';
import { DTS_PATH, MANIFEST_PATH, PLUGIN_ROOT } from './packages/cli/lib/paths.mjs';

let failures = 0;
function check(name, ok, detail = '') {
	if (ok) console.log(`PASS ${name}`);
	else {
		++failures;
		console.error(`FAIL ${name}${detail ? ` — ${detail}` : ''}`);
	}
}

const FIXTURES = join(PLUGIN_ROOT, 'Web', 'packages', 'cli', 'fixtures', 'lint');

// ---- 1. Lint fixtures, both ways. ----
for (const [file, rule] of [
	['bad-attribute-selector.rcss', 'bare-attribute-selector'],
	['bad-box-shadow-anim.rcss', 'box-shadow-animation'],
	['bad-layout-thrash.tsx', 'layout-thrash'],
]) {
	const findings = lintFile(join(FIXTURES, file));
	check(`lint: ${file} fires ${rule}`, findings.length > 0 && findings.every((f) => f.rule === rule),
		`got ${JSON.stringify(findings.map((f) => f.rule))}`);
}
for (const file of ['clean-attribute-selector.rcss', 'clean-box-shadow-anim.rcss', 'clean-layout-thrash.tsx']) {
	const findings = lintFile(join(FIXTURES, file));
	check(`lint: ${file} is silent`, findings.length === 0, `got ${JSON.stringify(findings)}`);
}

// ---- 2. Manifest freshness + the doctored-manifest detection. ----
check('manifest: committed file matches vacuus.d.ts', runManifest(DTS_PATH, MANIFEST_PATH, true) === 0);

{
	const scratch = mkdtempSync(join(tmpdir(), 'vacuus-smoke-'));
	try {
		const doctored = extractManifest(DTS_PATH);
		doctored.groups.vacuus = doctored.groups.vacuus.filter((n) => n !== 'translate');
		const doctoredPath = join(scratch, 'vacuus-api-manifest.json');
		writeFileSync(doctoredPath, manifestJson(doctored));
		check('manifest: a doctored manifest IS detected stale', runManifest(DTS_PATH, doctoredPath, true) === 1);
	} finally {
		rmSync(scratch, { recursive: true, force: true });
	}
}

// ---- 3. Create/build roundtrip (scratch DevUI; the app must sit inside the
//         workspace so @vacuus/preact resolves — cleaned up either way). ----
const appDir = join(PLUGIN_ROOT, 'Web', 'apps', 'smoketest');
const devui = mkdtempSync(join(tmpdir(), 'vacuus-smoke-devui-'));
try {
	rmSync(appDir, { recursive: true, force: true });
	check('create: scaffolds app + document halves',
		runCreate('smoketest', { devuiDir: devui }) === 0 &&
		existsSync(join(appDir, 'src', 'hud.tsx')) &&
		existsSync(join(devui, 'smoketest', 'smoketest.rml')) &&
		existsSync(join(devui, 'smoketest', 'vacuus-base.rcss')));

	check('create: the scaffolded RCSS lints clean',
		lintFile(join(devui, 'smoketest', 'smoketest.rcss')).length === 0 &&
		lintFile(join(devui, 'smoketest', 'vacuus-base.rcss')).length === 0);

	// The scaffolded document's <script src> must be the BARE bundle name: src is
	// document-relative (SystemInterface::JoinPath strips the filename and appends,
	// SystemInterface.cpp:72-83) and the document itself sits in DevUI/smoketest/,
	// so any directory component doubles it and the engine skips the script with one
	// Error — a scaffold with styling and no behaviour. Nothing else in this
	// roundtrip loads the document, so this regex is the only observable the
	// node-side smoke has for it (bead vjh).
	{
		const rml = readFileSync(join(devui, 'smoketest', 'smoketest.rml'), 'utf8');
		const srcs = [...rml.matchAll(/<script\s+src="([^"]*)"/g)].map((m) => m[1]);
		check('create: <script src> is the bare bundle name (no doubled directory)',
			srcs.length === 1 && srcs[0] === 'smoketest_bundle.js', JSON.stringify(srcs));
	}

	await runBuild(appDir, { devuiRoot: devui });
	const bundlePath = join(devui, 'smoketest', 'smoketest_bundle.js');
	const provenancePath = join(devui, 'smoketest', 'smoketest_bundle.provenance.json');
	check('build: bundle + inline sourcemap emitted',
		existsSync(bundlePath) && readFileSync(bundlePath, 'utf8').includes('sourceMappingURL=data:'));

	const provenance = existsSync(provenancePath) ? JSON.parse(readFileSync(provenancePath, 'utf8')) : {};
	check('build: provenance carries preact version + facade-manifest hash + command',
		provenance.preactVersion === '10.29.7' &&
		/^[0-9a-f]{40}$/.test(provenance.facadeManifestHash || '') &&
		(provenance.buildCommand || '').includes('vacuus.mjs build'),
		JSON.stringify(provenance));

	// ---- 4. Symbolicate over the roundtrip bundle. ----
	{
		const decoded = loadInlineMap(bundlePath);
		let mapped = null;
		let line = 0;
		for (let i = 1; i <= decoded.lines.length && !mapped; ++i) {
			const m = mapPosition(decoded, i, 1);
			if (m && m.includes('hud.tsx')) {
				mapped = m;
				line = i;
			}
		}
		check('symbolicate: a generated position maps into src/hud.tsx', mapped !== null);
		if (mapped) {
			const annotated = symbolicateStack(bundlePath, `    at App (vfs://smoketest/smoketest_bundle.js:${line}:1)`);
			check('symbolicate: stack lines get annotated', annotated.includes('-> ') && annotated.includes('hud.tsx'),
				annotated);
		}
	}
} finally {
	rmSync(appDir, { recursive: true, force: true });
	rmSync(devui, { recursive: true, force: true });
}

console.log(failures === 0 ? 'SMOKE GREEN' : `SMOKE RED: ${failures} failure(s)`);
process.exitCode = failures === 0 ? 0 : 1;
