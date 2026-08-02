// Copyright 2026 Vladimir Alyamkin. All Rights Reserved.
//
// VaCuus Reference HUD -- the JS third of the spec M6 2(h) driver split. C++
// data binding owns the scoreboard arrays, plate bars, ammo and objective;
// RCSS keyframes own the 18 buff sweeps; this file owns exactly three surfaces:
//
//  - 64 minimap blips, one rAF, ONE transform property written per blip per
//    frame (Exp-BLIP-DRIVER settled the idiom: a single transform write beats
//    left+top's two facade ops per blip and never dirties layout the way the
//    box properties do -- the measured numbers live in the driver test,
//    VaCuusRefHudTest.cpp, and the task report);
//  - killfeed churn via createElement/remove: one row per 1.5 s beat, trimmed
//    from the FRONT above 52 (12 in the clip window + 40 clipped history);
//  - damage numbers via timers over a FIXED pool of 24 recycled spans -- the
//    timer rotates values through the pool instead of churning nodes, so the
//    steady-state node count is exact.
//
// SERIAL-DETERMINISTIC, NO Math.random / Date.now (the M4 discipline,
// m4_hud_logic.js:9-13): every row, value and blip pose is a pure function of
// a serial or the rAF timestamp, so a screenshot at t is computable without
// running anything -- and two runs of the same length photograph the same HUD.
//
// THE WARM-UP IS THE BOOT (spec M6 2(g)): Exp-REF-COUNT is defined at declared
// steady state, and most of this composition materializes at runtime -- so
// boot() seeds the killfeed with its full 52-row history (serials 0..51, ~78
// sim-seconds of feed applied at once) and fills the damage pool to all 24
// slots. The HUD is AT steady state from its first frame; the live beats then
// hold the counts exactly (append+trim nets zero, pool slots recycle).
'use strict';

var KILLERS = ['RAPTOR', 'VIPER', 'GHOST', 'NOMAD', 'HAVOC'];
var WEAPONS = ['railgun', 'SMG', 'DMR', 'knife'];

var KILL_HISTORY = 52; // 12 live in the clip window + 40 clipped scrollback
var DMG_POOL = 24;
var NUM_BLIPS = 64;

var killSerial = 0;
var dmgSerial = 0;
var blips = [];

function two(n)
{
	return (n < 10 ? '0' : '') + n;
}

// One killfeed row from its serial -- the M4 parity arithmetic (pools 5/7/4
// pairwise coprime, HS on serial % 3 == 0). EXACTLY 8 nodes per row, always:
// the HS pill is a styled span with NO text (class 'hs' paints it), so a
// headshot row and a plain row count identically -- what makes 52 x 8 exact.
function appendKillRow(serial)
{
	var rows = document.getElementById('kf-rows');
	if (rows === null)
	{
		return;
	}

	var row = document.createElement('div');
	row.classList.add('kf-row');
	row.innerRML = '<span class="kf-killer">' + KILLERS[serial % 5] + '</span>'
		+ '<span class="kf-victim">BOGEY-' + two(serial % 7) + '</span>'
		+ '<span class="kf-weapon">' + WEAPONS[serial % 4] + '</span>'
		+ '<span class="kf-pill' + ((serial % 3) === 0 ? ' hs' : '') + '"></span>';
	rows.appendChild(row);

	// From the FRONT -- the expensive direction, deliberately (the M3b/M4
	// precedent): removing the first child shifts every survivor.
	while (rows.children.length > KILL_HISTORY)
	{
		rows.children[0].remove();
	}
}

function killBeat()
{
	appendKillRow(killSerial++);
}

// One damage value into pool slot serial % 24: text and crit class move, the
// NODES never do. Values 17..61 in steps of 11, crit every 4th (M4's table).
function dmgBeat()
{
	var zone = document.getElementById('dmg-layer');
	if (zone === null || zone.children.length < DMG_POOL)
	{
		return;
	}

	var serial = dmgSerial++;
	var slot = zone.children[serial % DMG_POOL];
	slot.innerRML = '-' + (17 + (serial % 5) * 11);
	if ((serial % 4) === 0)
	{
		slot.classList.add('dmg-crit');
	}
	else
	{
		slot.classList.remove('dmg-crit');
	}
}

// The blip tick: one 'transform' write per blip per frame. Poses are orbital
// -- radius/rate/phase all pure functions of the blip index -- around the
// 220x220 minimap's center; toFixed(1) keeps the strings short and stable.
function frame(tsMs)
{
	requestAnimationFrame(frame);

	var t = tsMs / 1000.0;
	for (var i = 0; i < blips.length; i++)
	{
		var radius = 18 + (i * 13) % 78;
		var rate = 0.2 + (i % 7) * 0.09;
		var phase = i * 2.399; // golden-angle spread, no two blips aligned
		var x = 107 + radius * Math.cos(t * rate + phase);
		var y = 107 + radius * Math.sin(t * rate + phase);
		blips[i].style.transform = 'translate(' + x.toFixed(1) + 'px, ' + y.toFixed(1) + 'px)';
	}
}

function boot()
{
	// 64 blips, friend/foe by index parity. left/top stay 0 -- position is the
	// transform's job from the first frame on, so the rAF writes ONE property.
	var mm = document.getElementById('minimap');
	if (mm !== null)
	{
		for (var i = 0; i < NUM_BLIPS; i++)
		{
			var b = document.createElement('div');
			b.classList.add('blip');
			if ((i % 2) === 0)
			{
				b.classList.add('friendly');
			}
			mm.appendChild(b);
			blips.push(b);
		}
	}

	// The killfeed history, saturated at boot: serials 0..51 through the SAME
	// append used live, so the trim logic and row shape are exercised, not
	// bypassed. The live 1.5 s beat continues from serial 52.
	for (var k = 0; k < KILL_HISTORY; k++)
	{
		appendKillRow(killSerial++);
	}

	// The damage pool: 24 spans built once, then dmgBeat recycles them.
	var zone = document.getElementById('dmg-layer');
	if (zone !== null)
	{
		for (var d = 0; d < DMG_POOL; d++)
		{
			var s = document.createElement('span');
			s.classList.add('dmg');
			zone.appendChild(s);
		}
		for (var w = 0; w < DMG_POOL; w++)
		{
			dmgBeat();
		}
	}

	setInterval(killBeat, 1500);
	setInterval(dmgBeat, 900);
	requestAnimationFrame(frame);
	console.log('refhud_logic: booted at steady state (64 blips, '
		+ killSerial + ' killfeed rows seeded, ' + DMG_POOL + '-slot damage pool)');
}

if (document !== null)
{
	boot();
}
