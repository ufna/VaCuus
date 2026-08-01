#!/usr/bin/env node
/*
 * @vacuus/cli — the VaCuus web toolchain (M5 Task 8, spec §3.1):
 *
 *   vacuus create <name>              scaffold a TSX app + its DevUI document
 *   vacuus build --app <dir>          bundle TSX -> Content/DevUI/<out>/, inline
 *                                     sourcemap, provenance manifest
 *   vacuus dev --app <dir>            watch mode riding the M2 live reload
 *   vacuus lint <files...>            the RCSS/facade gotcha rules
 *   vacuus symbolicate --bundle <js>  map engine-logged stack positions to TSX
 *                                     (reads the stack from stdin or --stack)
 *   vacuus manifest [--check]         (re)extract the typings manifest from
 *                                     vacuus.d.ts; --check fails when stale
 */

import { readFileSync } from 'node:fs';
import { runCreate } from '../lib/create.mjs';
import { runBuild, runDev } from '../lib/build.mjs';
import { runLint } from '../lib/lint.mjs';
import { runManifest } from '../lib/manifest.mjs';
import { symbolicateStack } from '../lib/symbolicate.mjs';
import { DTS_PATH, MANIFEST_PATH } from '../lib/paths.mjs';

function flag(args, name) {
	const i = args.indexOf(name);
	if (i === -1) return undefined;
	const value = args[i + 1];
	args.splice(i, 2);
	return value;
}

async function main() {
	const [command, ...args] = process.argv.slice(2);

	switch (command) {
		case 'create': {
			const appsDir = flag(args, '--apps-dir');
			const devuiDir = flag(args, '--devui-dir');
			if (args.length !== 1) break;
			return runCreate(args[0], { appsDir, devuiDir });
		}
		case 'build': {
			const app = flag(args, '--app') ?? process.cwd();
			const devuiRoot = flag(args, '--devui-dir');
			return runBuild(app, devuiRoot ? { devuiRoot } : {});
		}
		case 'dev': {
			const app = flag(args, '--app') ?? process.cwd();
			return runDev(app);
		}
		case 'lint': {
			if (args.length === 0) break;
			return runLint(args);
		}
		case 'symbolicate': {
			const bundle = flag(args, '--bundle');
			const stackFile = flag(args, '--stack');
			if (!bundle) break;
			const stack = stackFile ? readFileSync(stackFile, 'utf8') : readFileSync(0, 'utf8');
			console.log(symbolicateStack(bundle, stack));
			return 0;
		}
		case 'manifest': {
			return runManifest(DTS_PATH, MANIFEST_PATH, args.includes('--check'));
		}
		default:
			break;
	}

	console.error('usage: vacuus create <name> | build --app <dir> | dev --app <dir> | lint <files...> |');
	console.error('       symbolicate --bundle <js> [--stack <file>] | manifest [--check]');
	return 2;
}

process.exitCode = await main();
