// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.
//
// VaCuus M4 demo logic (spec 9): the M3 demo's per-frame driver, ported to JavaScript.
// The GAME still feeds the gameplay fields (Health, the scalars panel, the nested
// struct) through UpdateModel and data binding, unchanged -- this file owns only what
// spec 9 hands to JS: the second health bar, the killfeed, the damage numbers and the
// stance pill. Both paths coexisting on one document is the product's shape.
//
// SERIAL-DETERMINISTIC, NO Math.random, and the pool arithmetic below is the C++
// driver's, digit for digit (PumpDemoModel, VaCuusRender.cpp -- pools 5/7/4 pairwise
// coprime, HS on serial % 3 == 0, one row per 1.5 s beat, trim above 6 from the
// front). That identity is the parity claim a screenshot is checked against: the row
// content at serial S is computable without running anything.
'use strict';

var KILLERS = ['RAPTOR', 'VIPER', 'GHOST', 'NOMAD', 'HAVOC'];
var WEAPONS = ['railgun', 'SMG', 'DMR', 'knife'];
var STANCES = ['Standing', 'Crouched', 'Prone'];

// Minted ONCE: vacuus.model() returns a fresh {name, get} per call (documented no-identity),
// and the name resolves per get() against the live registry -- so a handle minted before the
// bind still reads correctly after it, and a per-frame mint would only be per-frame garbage.
var hudModel = vacuus.model('hud');

// Monotonic across Freeze/thaw, exactly like the C++ driver's KillfeedSerial: a thaw
// resumes the feed's CONTENT where it left off, never replays it.
var killSerial = 0;
var dmgSerial = 0;

var frozen = false;
var killTimer = 0;
var dmgTimer = 0;

// Change-gates for the per-frame rAF writes: RmlUi's own DataViewText skips the DOM
// when a value did not change (DataViewDefault.cpp:354), and the JS bar keeps the same
// manners -- an unconditional per-frame style write would dirty layout every frame for
// a value that moves visibly only a few times a second at this precision.
var lastBarWidth = null;
var lastStance = -1;

function two(n)
{
	return (n < 10 ? '0' : '') + n;
}

// One killfeed row from its serial -- THE parity arithmetic. innerRML rather than a
// span-by-span build because the row is created whole and never partially updated;
// classList for the row class (SetClass styles directly; the class ATTRIBUTE staying
// stale is the facade's documented trap and irrelevant to a write-only row).
function appendKillRow(serial)
{
	var rows = document.getElementById('kill-rows');
	if (rows === null)
	{
		return;
	}

	var row = document.createElement('div');
	row.classList.add('kill-row');
	row.innerRML = '<span class="killer">' + KILLERS[serial % 5] + '</span>'
		+ '<span class="k-sep">&#187;</span>'
		+ '<span class="victim">BOGEY-' + two(serial % 7) + '</span>'
		+ '<span class="weapon">' + WEAPONS[serial % 4] + '</span>'
		+ ((serial % 3) === 0 ? '<span class="hs">HS</span>' : '');
	rows.appendChild(row);

	// FROM THE FRONT, the C++ driver's own rule and for the same reason (M3b spec 3.6):
	// removing the first child shifts every survivor, the expensive direction on purpose.
	while (rows.children.length > 6)
	{
		rows.children[0].remove();
	}

	var count = document.getElementById('kill-count');
	if (count !== null)
	{
		count.innerRML = String(rows.children.length);
	}
}

function killBeat()
{
	appendKillRow(killSerial++);
}

// A damage number: born on its own 0.9 s interval, killed 1.2 s later by its own
// setTimeout -- the "timers" half of spec 9. Value and crit flag derive from the
// serial (17..61 in steps of 11, crit every 4th), so a screenshot's numbers are
// computable too. The removal timer keeps firing under Freeze by design: freezing
// stops NEW beats, in-flight deaths complete, and the zone drains empty within 1.2 s
// -- a frozen screen holds still instead of holding a corpse.
function dmgBeat()
{
	var zone = document.getElementById('dmg-zone');
	if (zone === null)
	{
		return;
	}

	var serial = dmgSerial++;
	var dmg = document.createElement('span');
	dmg.classList.add('dmg');
	if ((serial % 4) === 0)
	{
		dmg.classList.add('dmg-crit');
	}
	dmg.innerRML = '-' + (17 + (serial % 5) * 11);
	zone.appendChild(dmg);

	// dmg.remove() on an already-dead handle is a documented no-op (dead handles never
	// throw), so a trim or teardown racing this timer is harmless.
	setTimeout(function() { dmg.remove(); }, 1200);
}

// The per-frame half: the JS health bar (style proxy) and the stance pill (classList).
function frame(tsMs)
{
	requestAnimationFrame(frame);
	if (frozen)
	{
		// The C++ Freeze precedent, mirrored: the driver keeps calling UpdateModel with a
		// byte-identical struct every frame; this callback keeps running and mutates
		// nothing. Neither side publishes anything.
		return;
	}

	// Health comes FROM THE GAME: vacuus.M4Demo's reduced C++ driver sweeps it through
	// UpdateModel exactly as M3 did, and this read crosses the read surface into the
	// same UI shadow data binding renders from -- which is why the two bars must always
	// agree. The fallback sweep below (the C++ driver's own 10 s triangle) exists for
	// documents mounted WITHOUT a game feed -- the cost harness -- so the bar costs the
	// same per frame either way.
	var h = hudModel.get('Health');
	if (h === null)
	{
		var phase = (tsMs / 1000.0) % 10.0;
		h = phase < 5.0 ? 100.0 - phase * 20.0 : (phase - 5.0) * 20.0;
	}

	var width = h.toFixed(1) + '%';
	if (width !== lastBarWidth)
	{
		lastBarWidth = width;
		var bar = document.getElementById('js-bar-fill');
		if (bar !== null)
		{
			bar.style.width = width;
		}
	}

	// The stance pill, on the C++ driver's own 2 s beat (StanceIndex = int(t / 2) % 3)
	// -- driven from the rAF timestamp, which is RmlUi's animation clock, the same
	// wall-clock family the C++ driver reads.
	var stance = Math.floor(tsMs / 2000.0) % 3;
	if (stance !== lastStance)
	{
		lastStance = stance;
		var pill = document.getElementById('stance-val');
		if (pill !== null)
		{
			pill.classList.remove('stance-0');
			pill.classList.remove('stance-1');
			pill.classList.remove('stance-2');
			pill.classList.add('stance-' + stance);
			pill.innerRML = STANCES[stance];
		}
	}
}

function startBeats()
{
	if (killTimer === 0)
	{
		killTimer = setInterval(killBeat, 1500);
	}
	if (dmgTimer === 0)
	{
		dmgTimer = setInterval(dmgBeat, 900);
	}
}

// The Freeze surface (spec 9): vacuus.M4Demo.Freeze drives this through ExecuteScript.
// The honest JS freeze is PAUSING THE BEATS -- clearing the intervals stops the 1.5 s
// clock without firing it, where a fire-and-ignore interval would still count timer
// fires against the idle row. The rAF loop stays armed and inert (see frame()), the
// exact analogue of the C++ side's byte-identical UpdateModel. Thaw re-arms the
// intervals; the serials continue, so content resumes rather than replaying.
vacuus.onFreeze = function(isFrozen)
{
	frozen = !!isFrozen;
	if (frozen)
	{
		if (killTimer !== 0)
		{
			clearInterval(killTimer);
			killTimer = 0;
		}
		if (dmgTimer !== 0)
		{
			clearInterval(dmgTimer);
			dmgTimer = 0;
		}
	}
	else
	{
		startBeats();
	}
	console.log('m4_hud_logic: ' + (frozen ? 'frozen (beats paused, rAF inert)' : 'running (beats re-armed)'));
};

if (document !== null)
{
	startBeats();
	requestAnimationFrame(frame);
	console.log('m4_hud_logic: booted (killfeed beat 1.5s, damage beat 0.9s, bar+stance per frame)');
}
