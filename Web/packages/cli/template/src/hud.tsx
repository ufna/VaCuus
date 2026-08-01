/*
 * __APP_NAME__ — created by `vacuus create`.
 *
 * THE ONE STRUCTURAL RULE (E-P1): render at MODULE SCOPE into a DEDICATED EMPTY
 * mount element. Module scope, because a document reload destroys the whole JS
 * context and re-runs this module — top-level execution IS the re-mount path.
 * Dedicated and empty, because mounting into document/document.body throws by
 * construction on this facade, and pre-existing children of a non-empty mount
 * get adopted and re-propped by preact's excess-children scan.
 */

import { h, Fragment, render, useState, useEffect } from '@vacuus/preact';

function App() {
	const [count, setCount] = useState(0);

	useEffect(() => {
		vacuus.log('__APP_NAME__ mounted into view', vacuus.view.id);
	}, []);

	return (
		<div id="app-root">
			{/* User-visible strings route through the localization hook. Keys are
			    matched verbatim and a miss is identity, so gettext-style keys —
			    the English source text, {token}s included — degrade gracefully:
			    readable before the game pushes a table
			    (UVaCuusSubsystem::SetTranslationTable), translated after. */}
			<h1>{vacuus.translate('__APP_NAME__ ready')}</h1>
			<button
				class="bump"
				onClick={() => {
					setCount((v) => v + 1);
					vacuus.emit('bump', { count: count + 1 });
				}}
			>
				{vacuus.translate('Clicks: {count}', { count })}
			</button>
		</div>
	);
}

render(<App />, document!.getElementById('mount')!);
