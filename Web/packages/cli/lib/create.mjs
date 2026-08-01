/*
 * `vacuus create <name>` — scaffolds a TSX app in two halves that mirror how
 * the pieces are consumed:
 *
 *   Web/apps/<name>/            the TSX sources + tsconfig + a copy of
 *                               vacuus.d.ts (IDE-complete standalone)
 *   Content/DevUI/<name>/       the document skeleton (<name>.rml, <name>.rcss,
 *                               vacuus-base.rcss) — the engine-facing half,
 *                               where `vacuus build` also lands the bundle
 *
 * The template mandates the E-P1 shape: render at module scope into a dedicated
 * empty mount div (template/src/hud.tsx carries the argument).
 */

import { cpSync, mkdirSync, readFileSync, writeFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { DEVUI_ROOT, DTS_PATH, PLUGIN_ROOT, TEMPLATE_ROOT } from './paths.mjs';

function fill(template, name) {
	return template.replaceAll('__APP_NAME__', name).replaceAll('__APP_DIR__', name);
}

export function runCreate(name, { appsDir, devuiDir } = {}) {
	if (!/^[a-z][a-z0-9_-]*$/.test(name)) {
		console.error(`invalid app name '${name}': lowercase [a-z0-9_-], starting with a letter`);
		return 1;
	}

	appsDir = appsDir ?? join(PLUGIN_ROOT, 'Web', 'apps');
	devuiDir = devuiDir ?? DEVUI_ROOT;
	const appDir = join(appsDir, name);
	const docDir = join(devuiDir, name);
	if (existsSync(appDir) || existsSync(docDir)) {
		console.error(`refusing to overwrite: ${existsSync(appDir) ? appDir : docDir} already exists`);
		return 1;
	}

	const tpl = (file) => readFileSync(join(TEMPLATE_ROOT, file), 'utf8');

	// The app half.
	mkdirSync(join(appDir, 'src'), { recursive: true });
	mkdirSync(join(appDir, 'types'), { recursive: true });
	writeFileSync(join(appDir, 'package.json'), fill(tpl('package.json.tpl'), name));
	writeFileSync(join(appDir, 'src', 'hud.tsx'), fill(tpl('src/hud.tsx'), name));
	writeFileSync(join(appDir, 'tsconfig.json'), tpl('tsconfig.json'));
	writeFileSync(join(appDir, 'README.md'), fill(tpl('README.md.tpl'), name));
	writeFileSync(join(appDir, 'types', 'vacuus.d.ts'),
		'// Copied by `vacuus create` for standalone IDE typing.\n' +
		'// CANONICAL: Web/packages/cli/types/vacuus.d.ts (the manifest pipeline reads that one).\n\n' +
		readFileSync(DTS_PATH, 'utf8'));

	// The document half.
	mkdirSync(docDir, { recursive: true });
	writeFileSync(join(docDir, `${name}.rml`), fill(tpl('app.rml.tpl'), name));
	writeFileSync(join(docDir, `${name}.rcss`), fill(tpl('app.rcss.tpl'), name));
	cpSync(join(TEMPLATE_ROOT, 'vacuus-base.rcss'), join(docDir, 'vacuus-base.rcss'));

	console.log(`created ${appDir}`);
	console.log(`created ${docDir} (${name}.rml, ${name}.rcss, vacuus-base.rcss)`);
	console.log('next:');
	console.log('  (cd Web && npm install)                                  # once per checkout');
	console.log(`  node Web/packages/cli/bin/vacuus.mjs build --app ${join('Web', 'apps', name)}`);
	console.log(`  # load '${name}/${name}.rml' in a VaCuus view`);
	return 0;
}
