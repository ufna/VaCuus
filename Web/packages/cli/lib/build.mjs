/*
 * `vacuus build` / `vacuus dev` — esbuild over a TSX app into Content/DevUI,
 * where the M2 live-reload watcher already watches (roots + js|mjs extensions,
 * VaCuusLiveReload.cpp:39-40): a dev-mode rebuild lands the bundle and the
 * running editor reloads the document — full re-mount by module top level.
 *
 * THE TSX-ERROR TRAP, printed loudly on every watch failure because it is the
 * spec's named silence (§2(l)): a failed rebuild writes NO bundle, so NO file
 * event fires, so the engine keeps the LAST GOOD UI with zero in-engine
 * indication. The terminal is the only place the failure exists.
 *
 * Provenance (spec §2(l), the committed-bundle staleness protocol): every build
 * writes <bundle>.provenance.json beside the bundle — preact version, the
 * facade-manifest hash, the reproduction command — and the in-engine bundle
 * test SKIPS with a named warning when the hash no longer matches the current
 * facade manifest, turning "undiagnosable red after a facade change" into
 * "rebuild instruction".
 */

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { createRequire } from 'node:module';
import { join, resolve, relative, dirname } from 'node:path';
import { DEVUI_ROOT, PLUGIN_ROOT, facadeManifestHash } from './paths.mjs';

/** Reads the app's package.json and its `vacuus` config block. */
export function appConfig(appDir) {
	const pkgPath = join(appDir, 'package.json');
	const pkg = JSON.parse(readFileSync(pkgPath, 'utf8'));
	const cfg = pkg.vacuus;
	if (!cfg || !cfg.entry || !cfg.outDir || !cfg.bundleName) {
		throw new Error(
			`${pkgPath}: needs a "vacuus" block { entry, outDir, bundleName } — ` +
			`entry is the TSX module, outDir the Content/DevUI subdirectory, bundleName the emitted file`);
	}
	return { pkg, cfg };
}

/** preact's version as the app resolves it — the provenance's and the banner's shared truth. */
function resolvedPreactVersion(appDir) {
	const require = createRequire(join(appDir, 'package.json'));
	return JSON.parse(readFileSync(require.resolve('preact/package.json'), 'utf8')).version;
}

/**
 * Every emitted bundle embeds preact, so every bundle must carry preact's MIT
 * notice — the license's "included in all copies or substantial portions"
 * condition. The text comes from the COMMITTED copy beside the adapter
 * (Web/packages/preact-vacuus/PREACT-LICENSE, the Source/ThirdParty
 * convention), not from node_modules, so the shipped notice is the reviewed
 * one. `/*!` marks it minifier-preserved; esbuild offsets the inline
 * sourcemap past its own banner, so symbolicate stays exact.
 */
function licenseBanner(appDir) {
	const licensePath = join(PLUGIN_ROOT, 'Web', 'packages', 'preact-vacuus', 'PREACT-LICENSE');
	const license = readFileSync(licensePath, 'utf8').trimEnd();
	const body = license.replace(/^/gm, ' * ').replace(/[ \t]+$/gm, '');
	return `/*!\n * Bundles preact ${resolvedPreactVersion(appDir)} — https://github.com/preactjs/preact\n *\n${body}\n */`;
}

function esbuildOptions(appDir, cfg, outfile) {
	return {
		entryPoints: [join(appDir, cfg.entry)],
		outfile,
		bundle: true,
		banner: { js: licenseBanner(appDir) },
		// IIFE: the bundle runs as a captured classic <script src> through the
		// document path (XMLNodeHandlerHead script capture), not as an ES module.
		format: 'iife',
		// Classic JSX transform against @vacuus/preact's re-exported h/Fragment —
		// the template imports them explicitly, so no inject is needed.
		jsx: 'transform',
		jsxFactory: 'h',
		jsxFragment: 'Fragment',
		// Inline maps: the shipped sourcemap tier (arch correction #5) — the raw
		// generated positions in engine logs resolve offline via `vacuus symbolicate`.
		sourcemap: 'inline',
		// quickjs-ng is comfortably past ES2020; higher targets buy syntax the
		// engine-side parser has not been soak-tested with.
		target: 'es2020',
		charset: 'utf8',
		logLevel: 'silent',
	};
}

function writeProvenance(appDir, cfg, outfile) {
	const provenance = {
		preactVersion: resolvedPreactVersion(appDir),
		facadeManifestHash: facadeManifestHash(),
		buildCommand: `node Web/packages/cli/bin/vacuus.mjs build --app ${relative(PLUGIN_ROOT, appDir)}`,
	};
	const provenancePath = outfile.replace(/\.js$/, '.provenance.json');
	writeFileSync(provenancePath, JSON.stringify(provenance, null, 2) + '\n');
	return provenancePath;
}

export async function runBuild(appDir, { devuiRoot = DEVUI_ROOT } = {}) {
	const esbuild = await import('esbuild');
	appDir = resolve(appDir);
	const { cfg } = appConfig(appDir);
	const outfile = join(devuiRoot, cfg.outDir, cfg.bundleName);
	mkdirSync(dirname(outfile), { recursive: true });

	await esbuild.build(esbuildOptions(appDir, cfg, outfile));
	const provenancePath = writeProvenance(appDir, cfg, outfile);
	console.log(`built ${outfile}`);
	console.log(`provenance ${provenancePath}`);
	return 0;
}

export async function runDev(appDir) {
	const esbuild = await import('esbuild');
	appDir = resolve(appDir);
	const { cfg } = appConfig(appDir);
	const outfile = join(DEVUI_ROOT, cfg.outDir, cfg.bundleName);
	mkdirSync(dirname(outfile), { recursive: true });

	const options = esbuildOptions(appDir, cfg, outfile);
	options.plugins = [
		{
			name: 'vacuus-dev-banner',
			setup(build) {
				build.onEnd((result) => {
					const stamp = new Date().toLocaleTimeString();
					if (result.errors.length > 0) {
						console.error('');
						console.error('='.repeat(72));
						console.error(`[${stamp}] BUILD FAILED — ${result.errors.length} error(s). READ THIS TERMINAL:`);
						console.error('  no bundle was written, so NO reload fires and the engine keeps');
						console.error('  showing the LAST GOOD UI. Nothing in-engine will tell you.');
						console.error('='.repeat(72));
						for (const e of result.errors) {
							const loc = e.location ? `${e.location.file}:${e.location.line}:${e.location.column}: ` : '';
							console.error(`  ${loc}${e.text}`);
						}
					} else {
						writeProvenance(appDir, cfg, outfile);
						console.log(`[${stamp}] rebuilt ${outfile} — the M2 watcher reloads it now (full re-mount)`);
					}
				});
			},
		},
	];

	const context = await esbuild.context(options);
	await context.watch();
	console.log(`watching ${join(appDir, cfg.entry)} -> ${outfile}`);
	console.log('TSX errors reach ONLY this terminal (failed builds write no bundle => no reload => the engine keeps the last good UI).');
	// Watch until killed.
	await new Promise(() => {});
}
