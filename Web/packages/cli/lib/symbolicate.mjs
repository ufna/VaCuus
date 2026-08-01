/*
 * `vacuus symbolicate` — offline stack-trace mapping over the INLINE sourcemap
 * esbuild embeds in every `vacuus build`/`vacuus dev` bundle. This is the M5
 * sourcemap scope as honestly rescoped (spec §2(l), arch correction #5): the
 * engine logs RAW generated positions (vfs://…/hud_bundle.js:line:col); paste
 * them here to get original TSX positions. In-engine resolution is v1.x.
 *
 * Self-contained decoder — no `source-map` npm dependency: the v3 mappings
 * grammar is base64 VLQ segments [genCol, srcIdx, srcLine, srcCol, nameIdx],
 * all but genCol cumulative across the whole map, genCol resetting per ';'
 * (generated line). ~60 lines beats a dependency the Fab source-only rule
 * would make every consumer install to read a stack.
 */

import { readFileSync } from 'node:fs';

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

/** Decodes one mappings line ';'-chunk into segments, with cross-line state carried in `state`. */
function decodeLine(chunk, state) {
	const segments = [];
	let genCol = 0;
	for (const seg of chunk.split(',')) {
		if (!seg) continue;
		const fields = [];
		let value = 0;
		let shift = 0;
		for (const ch of seg) {
			const digit = B64.indexOf(ch);
			if (digit === -1) throw new Error(`bad VLQ character '${ch}'`);
			value |= (digit & 31) << shift;
			if (digit & 32) {
				shift += 5;
			} else {
				fields.push(value & 1 ? -(value >>> 1) : value >>> 1);
				value = 0;
				shift = 0;
			}
		}
		genCol += fields[0];
		if (fields.length >= 4) {
			state.srcIdx += fields[1];
			state.srcLine += fields[2];
			state.srcCol += fields[3];
			if (fields.length >= 5) state.nameIdx += fields[4];
			segments.push({
				genCol,
				srcIdx: state.srcIdx,
				srcLine: state.srcLine,
				srcCol: state.srcCol,
				nameIdx: fields.length >= 5 ? state.nameIdx : -1,
			});
		}
	}
	return segments;
}

/** Pulls and parses the inline sourcemap out of a built bundle. */
export function loadInlineMap(bundlePath) {
	const text = readFileSync(bundlePath, 'utf8');
	const m = text.match(/\/\/# sourceMappingURL=data:application\/json;base64,([A-Za-z0-9+/=]+)/);
	if (!m) {
		throw new Error(
			`${bundlePath} carries no inline sourcemap — was it built by \`vacuus build\` (which always inlines)?`);
	}
	const map = JSON.parse(Buffer.from(m[1], 'base64').toString('utf8'));
	const state = { srcIdx: 0, srcLine: 0, srcCol: 0, nameIdx: 0 };
	const lines = map.mappings.split(';').map((chunk) => decodeLine(chunk, state));
	return { map, lines };
}

/** Maps a 1-based generated (line, col?) to "source:line:col (name)" or null. */
export function mapPosition({ map, lines }, line, col) {
	const segments = lines[line - 1];
	if (!segments || segments.length === 0) return null;
	let best = segments[0];
	if (col !== undefined) {
		for (const seg of segments) {
			if (seg.genCol <= col - 1) best = seg;
			else break;
		}
	}
	const source = map.sources[best.srcIdx] ?? '<unknown>';
	const name = best.nameIdx >= 0 ? ` (${map.names[best.nameIdx]})` : '';
	return `${source}:${best.srcLine + 1}:${best.srcCol + 1}${name}`;
}

/**
 * Annotates every stack line that names a .js position. QuickJS backtraces look
 * like "    at funcName (vfs://M5Hud/hud_bundle.js:12:34)" — column optional.
 */
export function symbolicateStack(bundlePath, stackText) {
	const decoded = loadInlineMap(bundlePath);
	return stackText
		.split('\n')
		.map((line) => {
			const m = line.match(/([^\s()]+\.js):(\d+)(?::(\d+))?/);
			if (!m) return line;
			const mapped = mapPosition(decoded, Number(m[2]), m[3] !== undefined ? Number(m[3]) : undefined);
			return mapped ? `${line}  -> ${mapped}` : line;
		})
		.join('\n');
}
