/* Clean fixture for the layout-thrash rule: the per-frame callback drives
   transform (render-level, no layout) and the one width write is a one-time
   setup outside any per-frame callback. Web/smoke.mjs asserts SILENT. */
declare const bar: { style: Record<string, string> };

bar.style.width = '100px'; // one-time setup: fine

requestAnimationFrame(function tick() {
	bar.style.transform = 'translateX(' + Math.random() * 10 + 'px)';
	requestAnimationFrame(tick);
});
