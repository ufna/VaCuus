/* Known-bad fixture for the layout-thrash rule: layout-driving style writes
   inside per-frame callbacks. Web/smoke.mjs asserts this file FIRES.
   (Fixture only — never bundled.) */
declare const bar: { style: Record<string, string> } & { style: { setProperty(n: string, v: string): boolean } };

requestAnimationFrame(function tick() {
	bar.style.width = Math.random() * 100 + 'px';
	requestAnimationFrame(tick);
});

setInterval(() => {
	bar.style.setProperty('height', '20px');
}, 16);
