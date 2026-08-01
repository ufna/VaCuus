/*
 * Repo-layout resolution for the CLI. The layout is fixed by the plugin:
 *   <plugin>/Web/packages/cli/…      (this package)
 *   <plugin>/Content/DevUI/          (the watcher roots — arch correction #3;
 *                                     `vacuus dev|build` writes bundles here)
 *   <plugin>/Content/DevUI/Tests/vacuus-api-manifest.json  (the committed
 *                                     typings manifest, spec §2(l))
 */

import { fileURLToPath } from 'node:url';
import { dirname, join, resolve } from 'node:path';
import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';

const CLI_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');

export const PLUGIN_ROOT = resolve(CLI_ROOT, '..', '..', '..');
export const DEVUI_ROOT = join(PLUGIN_ROOT, 'Content', 'DevUI');
export const MANIFEST_PATH = join(DEVUI_ROOT, 'Tests', 'vacuus-api-manifest.json');
export const DTS_PATH = join(CLI_ROOT, 'types', 'vacuus.d.ts');
export const TEMPLATE_ROOT = join(CLI_ROOT, 'template');

/**
 * SHA-1 hex of the committed manifest's raw bytes — the provenance link between
 * a built bundle and the facade surface it was typed against. The in-engine
 * test computes the same digest over the same file (FSHA1), so the two sides
 * agree byte-for-byte or the bundle test skips with its named warning.
 */
export function facadeManifestHash() {
	return createHash('sha1').update(readFileSync(MANIFEST_PATH)).digest('hex');
}
