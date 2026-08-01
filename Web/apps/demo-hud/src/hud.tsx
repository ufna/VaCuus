/*
 * The M5 TSX HUD (spec §8): the committed demo bundle's source. Built by
 * `vacuus build --app Web/apps/demo-hud` into Content/DevUI/M5Hud/hud_bundle.js
 * (+ provenance manifest); the committed bundle is what the in-engine test
 * VaCuus.Js.Preact.BundleMount loads and what vacuus.M5Demo shows.
 *
 * Rendered at MODULE SCOPE into the dedicated empty #mount div — the E-P1
 * mandate; module top level is the reload re-mount path.
 *
 * DETERMINISTIC ON PURPOSE: the killfeed cycles a fixed roster and the health
 * fallback is a fixed sweep, so the in-engine DOM probes and the AutoShot
 * screenshots assert stable content. Steady-state cost mirrors the measured M4
 * shape (m4_hud_logic.js): one rAF per frame reading the model with a
 * change-gated commit, plus a 1.5 s interval beat.
 */

import { h, Fragment, render, useState, useEffect } from '@vacuus/preact';

const ROSTER = ['Vex', 'Kilo', 'Moth', 'Rasp', 'Unto'];

function Killfeed({ rows }: { rows: { id: number; killer: string; victim: string }[] }) {
	return (
		<div id="killfeed">
			{rows.map((r) => (
				<div key={r.id} class="kill-row">
					{/* Gettext-style key: identity-readable, table-translatable; the
					    tokens substitute either way. */}
					{vacuus.translate('{killer} downed {victim}', { killer: r.killer, victim: r.victim })}
				</div>
			))}
		</div>
	);
}

function Hud() {
	const [health, setHealth] = useState<number | null>(null);
	const [rows, setRows] = useState(() => [
		{ id: 1, killer: ROSTER[0], victim: ROSTER[1] },
		{ id: 2, killer: ROSTER[2], victim: ROSTER[3] },
	]);

	// The M4 demo's one-value-two-paths pattern: the game feeds 'hud'.Health via
	// data models; this HUD reads the same UI shadow per frame, CHANGE-GATED so a
	// static value costs a read and no render. Unbound (test rig, no game) reads
	// null -> the fixed fallback sweep keeps the bar alive and deterministic.
	useEffect(() => {
		let raf = 0;
		let tick = 0;
		const loop = () => {
			const value = vacuus.model('hud').get('Health');
			++tick;
			const next = typeof value === 'number' ? Math.round(value) : 50 + Math.round(25 * Math.sin(tick / 60));
			setHealth((prev) => (prev === next ? prev : next));
			raf = requestAnimationFrame(loop);
		};
		raf = requestAnimationFrame(loop);
		return () => cancelAnimationFrame(raf);
	}, []);

	// The killfeed beat: one deterministic row per 1.5 s, capped at 5 — the
	// amortized-interval shape the M4 steady-state row measured.
	useEffect(() => {
		const beat = setInterval(() => {
			setRows((old) => {
				const id = old.length > 0 ? old[old.length - 1].id + 1 : 1;
				return [...old.slice(-4), { id, killer: ROSTER[id % 5], victim: ROSTER[(id + 2) % 5] }];
			});
		}, 1500);
		return () => clearInterval(beat);
	}, []);

	const shown = health ?? 0;
	return (
		<div id="hud-root">
			<h1 id="hud-title">{vacuus.translate('VaCuus M5 // TSX HUD')}</h1>
			<div class="panel">
				<div class="row">
					<span class="key">{vacuus.translate('Health')}</span>
					<span class="val" id="health-val">{shown}</span>
				</div>
				<div id="health-bar">
					<div id="health-bar-fill" style={{ width: shown + '%' }} />
				</div>
			</div>
			<div class="panel">
				<div class="row">
					<span class="key">{vacuus.translate('Killfeed')}</span>
					<button
						id="sim-btn"
						onClick={() => {
							setRows((old) => {
								const id = old.length > 0 ? old[old.length - 1].id + 1 : 1;
								return [...old.slice(-4), { id, killer: 'You', victim: ROSTER[id % 5] }];
							});
							vacuus.emit('sim_kill', { by: 'button' });
						}}
					>
						{vacuus.translate('Simulate')}
					</button>
				</div>
				<Killfeed rows={rows} />
			</div>
		</div>
	);
}

render(<Hud />, document!.getElementById('mount')!);
