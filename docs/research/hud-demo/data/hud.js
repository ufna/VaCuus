// VaCuus HUD demo - fake combat sim driving the RmlUi document through the
// minimal `vacuus` binding surface. Runs at display rate via rAF.
'use strict';

// ---------------------------------------------------------------- utils ----
function mulberry32(seed) {
	let a = seed >>> 0;
	return function () {
		a |= 0; a = (a + 0x6D2B79F5) | 0;
		let t = Math.imul(a ^ (a >>> 15), 1 | a);
		t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
		return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
	};
}
const rand = mulberry32(0xC0FFEE);
const R  = (a, b) => a + rand() * (b - a);
const RI = (a, b) => Math.floor(R(a, b + 1));
const pick = (arr) => arr[RI(0, arr.length - 1)];

const AUTO = (typeof AUTO_EXIT_SECONDS === 'number' && AUTO_EXIT_SECONDS > 0);
const view = vacuus.contextSize();
const el = (id) => vacuus.getElementById(id);

const counters = { dmg: 0, kf: 0, casts: 0, frames: 0 };

// ---------------------------------------------------------- health / mana --
let hp = 100, mp = 100;
const hpFill = el('hp-fill'), hpText = el('hp-text'), hpTrack = el('hp-track');
const mpFill = el('mp-fill'), mpText = el('mp-text');

function refreshBars() {
	hpFill.setProperty('width', Math.max(0, hp).toFixed(1) + '%');
	mpFill.setProperty('width', Math.max(0, mp).toFixed(1) + '%');
	hpText.setInnerRML(String(Math.round(hp)));
	mpText.setInnerRML(String(Math.round(mp)));
	hpTrack.setClass('low', hp < 30);
}

function takeDamage(amount) {
	hp = Math.max(1, hp - amount); // never die in the demo
	refreshBars();
	if (amount >= 12) el('vignette').setClass('hit', true);
}
// retrigger: clear the vignette animation class shortly after each flash
setInterval(() => el('vignette').setClass('hit', false), 700);

(function incomingLoop() {
	setTimeout(() => {
		if (rand() < 0.18) { hp = Math.min(100, hp + RI(8, 20)); spawnDamage(true); refreshBars(); }
		else takeDamage(RI(4, 19));
		incomingLoop();
	}, R(700, 2100));
})();

// ----------------------------------------------------- damage numbers ------
function spawnDamage(isHeal) {
	const crit = !isHeal && rand() < 0.22;
	const val = isHeal ? '+' + RI(8, 20) : String(crit ? RI(180, 999) : RI(12, 160));
	const d = vacuus.createElementIn('damage-layer', 'div', val);
	if (!d) return;
	d.setAttribute('class', 'dmg' + (crit ? ' crit' : '') + (isHeal ? ' heal' : ''));
	const x = isHeal ? R(120, 380) : R(view.w * 0.32, view.w * 0.68);
	const y = isHeal ? R(120, 200) : R(view.h * 0.24, view.h * 0.58);
	d.setProperty('left', x.toFixed(0) + 'px');
	d.setProperty('top', y.toFixed(0) + 'px');
	counters.dmg++;
	setTimeout(() => d.remove(), 1000);
	if (crit) hitmarker();
}

(function outgoingLoop() {
	setTimeout(() => {
		spawnDamage(false);
		if (rand() < 0.35) hitmarker();
		outgoingLoop();
	}, R(240, 640));
})();

// ------------------------------------------------------------ hitmarker ----
const hm = el('hitmarker');
let hmTimer = 0;
function hitmarker() {
	hm.setClass('show', false);
	// re-add on the next frame so the animation restarts
	requestAnimationFrame(() => hm.setClass('show', true));
	clearTimeout(hmTimer);
	hmTimer = setTimeout(() => hm.setClass('show', false), 320);
}

// ------------------------------------------------------------- killfeed ----
const NAMES = ['ufna', 'Nyx-7', 'VOIDWALKER', 'p1x3l', 'Kessler', 'Moth', 'DrNode', 'Rml_Enjoyer'];
const WEAPONS = ['railgun', 'pulse SMG', 'void blade', 'nano swarm', 'arc caster'];
const kfLive = [];

function pushKill() {
	let a = pick(NAMES), b = pick(NAMES);
	while (b === a) b = pick(NAMES);
	const row = vacuus.createElementIn('killfeed', 'div',
		'<span class="kf-killer">' + a + '</span> <span class="kf-weapon">[' + pick(WEAPONS) + ']</span> <span class="kf-victim">' + b + '</span>');
	if (!row) return;
	row.setAttribute('class', 'kf-row' + (a === 'ufna' ? ' kf-you' : ''));
	requestAnimationFrame(() => row.setClass('shown', true)); // trigger slide-in transition
	kfLive.push(row);
	counters.kf++;
	setTimeout(() => row.setClass('fade', true), 4000);
	setTimeout(() => { row.remove(); const i = kfLive.indexOf(row); if (i >= 0) kfLive.splice(i, 1); }, 4600);
	if (kfLive.length > 6) kfLive[0].setClass('fade', true); // cap visible rows
}
(function killfeedLoop() {
	setTimeout(() => { pushKill(); killfeedLoop(); }, R(1600, 3400));
})();

// ------------------------------------------------------------- abilities ---
const abilities = [4, 6, 9, 14].map((max, i) => ({
	max: max, left: 0, cost: 8 + i * 7,
	slot: el('slot-' + (i + 1)), fill: el('cd-' + (i + 1)),
	text: el('cdt-' + (i + 1)), flash: el('flash-' + (i + 1)),
}));
abilities[3].slot.setClass('ready', true);

function cast(i) {
	const ab = abilities[i];
	if (ab.left > 0 || mp < ab.cost) return;
	mp -= ab.cost;
	ab.left = ab.max;
	ab.slot.setClass('cooling', true);
	if (i === 3) ab.slot.setClass('ready', false);
	counters.casts++;
	spawnDamage(false);
	hitmarker();
	refreshBars();
}
abilities.forEach((ab, i) => ab.slot.addEventListener('click', () => cast(i)));

function tickAbilities(dt) {
	abilities.forEach((ab, i) => {
		if (ab.left <= 0) return;
		ab.left -= dt;
		if (ab.left <= 0) {
			ab.left = 0;
			ab.slot.setClass('cooling', false);
			ab.fill.setProperty('height', '0%');
			ab.text.setInnerRML('');
			ab.flash.setClass('on', false);
			requestAnimationFrame(() => ab.flash.setClass('on', true));
			setTimeout(() => ab.flash.setClass('on', false), 500);
			if (i === 3) ab.slot.setClass('ready', true);
		} else {
			ab.fill.setProperty('height', (ab.left / ab.max * 100).toFixed(1) + '%');
			ab.text.setInnerRML(ab.left > 3 ? String(Math.ceil(ab.left)) : ab.left.toFixed(1));
		}
	});
}

// --------------------------------------------------------------- minimap ---
const MM_C = 95, MM_DOT = 3.5;
const blips = [];
for (let i = 0; i < 5; i++)
	blips.push({ enemy: true, ang: R(0, 6.28), r: R(28, 78), spd: R(0.25, 0.7) * (rand() < 0.5 ? -1 : 1), el: null });
for (let i = 0; i < 3; i++)
	blips.push({ enemy: false, ang: R(0, 6.28), r: R(18, 60), spd: R(0.15, 0.4) * (rand() < 0.5 ? -1 : 1), el: null });
blips.forEach((b) => {
	b.el = vacuus.createElementIn('mm-blips', 'div', '');
	b.el.setAttribute('class', 'blip ' + (b.enemy ? 'blip-enemy' : 'blip-ally'));
});
function tickMinimap(t, dt) {
	blips.forEach((b, i) => {
		b.ang += b.spd * dt;
		const r = b.r + Math.sin(t * 0.8 + i * 1.7) * 7;
		b.el.setProperty('left', (MM_C + Math.cos(b.ang) * r - MM_DOT).toFixed(1) + 'px');
		b.el.setProperty('top', (MM_C + Math.sin(b.ang) * r - MM_DOT).toFixed(1) + 'px');
	});
}

// --------------------------------------------------------------- settings --
let settingsOpen = false;
function toggleSettings(force) {
	settingsOpen = (force === undefined) ? !settingsOpen : force;
	el('settings').setClass('open', settingsOpen);
	el('settings-backdrop').setClass('open', settingsOpen);
}
el('settings-close').addEventListener('click', () => toggleSettings(false));

// ------------------------------------------------------------------ keys ---
vacuus.onKey = function (key) {
	if (key >= '1' && key <= '4') cast(Number(key) - 1);
	else if (key === 'Escape') toggleSettings();
	else if (key === 'Space') { // burst fire
		spawnDamage(false); spawnDamage(false); hitmarker();
	}
};

// ------------------------------------------------------------- perf HUD ----
setInterval(() => {
	const s = vacuus.stats();
	el('perf').setInnerRML(
		'fps ' + s.fps.toFixed(0) +
		' | update ' + s.updateMs.toFixed(2) + ' ms' +
		' | render ' + s.renderMs.toFixed(2) + ' ms');
}, 250);

// ------------------------------------------------------------- main loop ---
let last = -1;
function frame(tsMs) {
	const t = tsMs / 1000;
	if (last < 0) last = t;
	const dt = Math.min(t - last, 0.1);
	last = t;
	counters.frames++;

	mp = Math.min(100, mp + 5.5 * dt);
	hp = Math.min(100, hp + 1.2 * dt);
	if ((counters.frames & 15) === 0) refreshBars();

	tickAbilities(dt);
	tickMinimap(t, dt);
	requestAnimationFrame(frame);
}
refreshBars();
requestAnimationFrame(frame);

// ----------------------------------------------------------- auto (smoke) --
if (AUTO) {
	vacuus.log('AUTO mode: exiting after ' + AUTO_EXIT_SECONDS + 's');
	// scripted inputs so cooldowns / settings / burst all get exercised
	setTimeout(() => vacuus.onKey('1'), 900);
	setTimeout(() => vacuus.onKey('2'), 1400);
	setTimeout(() => vacuus.onKey('3'), 1900);
	setTimeout(() => vacuus.onKey('4'), 2300);
	setTimeout(() => vacuus.onKey('Space'), 2700);
	setTimeout(() => vacuus.onKey('Escape'), 3100);
	setTimeout(() => vacuus.onKey('Escape'), 4100);
	let sec = 0;
	setInterval(() => {
		sec++;
		const s = vacuus.stats();
		vacuus.log('t=' + sec + 's fps=' + s.fps.toFixed(1) +
			' update=' + s.updateMs.toFixed(2) + 'ms render=' + s.renderMs.toFixed(2) + 'ms' +
			' frames=' + counters.frames + ' dmg=' + counters.dmg +
			' killfeed=' + counters.kf + ' casts=' + counters.casts + ' hp=' + Math.round(hp));
	}, 1000);
	setTimeout(() => { vacuus.log('AUTO done, requesting exit'); vacuus.exit(); }, AUTO_EXIT_SECONDS * 1000);
}

vacuus.log('hud.js loaded: view=' + view.w + 'x' + view.h + ' auto=' + AUTO);
