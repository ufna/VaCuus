/*
 * The @vacuus/preact test fixture (M5 Task 8) — the ADAPTED twin of Task 1's
 * stock fixtures (fixture-dom.js/fixture-counter.js pin what stock preact does
 * against the facade; THIS one pins what the adapter fixes). Built by
 * `vacuus build --app Web/apps/ep-fixture` into
 * Content/DevUI/Tests/fixture-vacuus-preact.js and driven by:
 *
 *  - VaCuus.Js.Preact.AdapterContract — the E-P2 pair through the adapter:
 *    onClick fires on RmlUi's lowercase "click" (stock registered dead "Click"),
 *    and className lands as the class ATTRIBUTE (stock wrote a dead "className"
 *    attribute) — both recorded in ep-observations.md;
 *  - VaCuus.Js.Preact.DesyncObservability (E-P6) — bump()/pushRow() after a C++
 *    SetInnerRML killed the tree;
 *  - VaCuus.Js.Preact.ReloadRemount (E-P7) — RUNS counts module top-level
 *    executions per context; listeners registered here must return to zero
 *    after a recycle.
 */

import { h, render, useState } from '@vacuus/preact';

const g = globalThis as any;

function Probe() {
	const [n, setN] = useState(0);
	const [rows, setRows] = useState(['r0', 'r1']);
	g.bump = () => setN((v: number) => v + 1);
	g.pushRow = (r: string) => setRows((v: string[]) => [...v, r]);
	return (
		<div id="probe-root">
			<button id="probe-btn" onClick={() => setN((v: number) => v + 1)}>
				n:{n}
			</button>
			<div id="probe-cn" className="via-class-name">cn</div>
			<div id="probe-rows">
				{rows.map((r) => (
					<div key={r} class="row">{r}</div>
				))}
			</div>
		</div>
	);
}

// Fresh per JS context by construction: the module cache dies with the context
// (VaCuusJsModules cache note), so RUNS > 1 within one context would mean the
// bundle ran twice WITHOUT a recycle.
g.RUNS = (g.RUNS || 0) + 1;

render(<Probe />, document!.getElementById('mount')!);
