import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { build, version as esbuildVersion } from 'esbuild';

const require = createRequire(new URL('./packages/preact-vacuus/package.json', import.meta.url));
const preactVersion = JSON.parse(readFileSync(require.resolve('preact/package.json'), 'utf8')).version;
const license = readFileSync(new URL('./packages/preact-vacuus/PREACT-LICENSE', import.meta.url), 'utf8').trimEnd();
const banner = `/*!
 * VaCuus stock Preact fixtures: preact ${preactVersion}, esbuild ${esbuildVersion}.
 * Rebuild: node Web/build-stock-fixtures.mjs
 * Sources: Web/apps/ep-fixture/src/stock-{dom,counter}.js
 *
${license.replace(/^/gm, ' * ').replace(/[ \t]+$/gm, '')}
 */`;

for (const name of ['dom', 'counter']) {
	await build({
		entryPoints: [fileURLToPath(new URL(`./apps/ep-fixture/src/stock-${name}.js`, import.meta.url))],
		outfile: fileURLToPath(new URL(`../Content/DevUI/Tests/fixture-${name}.js`, import.meta.url)),
		bundle: true,
		format: 'iife',
		target: 'es2020',
		banner: { js: banner },
	});
}
