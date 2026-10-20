// Render the site's social card from the shared thumbnail layout. The Fab
// listing has its own release cadence; the site's test count comes from Home.vue.
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const home = readFileSync(new URL('../site/.vitepress/theme/Home.vue', import.meta.url), 'utf8');
const count = home.match(/(\d+)-test automation suite/)[1];
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE || 'playwright');
const browser = await chromium.launch({
	headless: true,
	...(process.env.CHROMIUM_EXECUTABLE ? { executablePath: process.env.CHROMIUM_EXECUTABLE } : {}),
});
try {
	const page = await browser.newPage({ viewport: { width: 1920, height: 1080 }, deviceScaleFactor: 1 });
	const origin = process.env.CARD_ORIGIN || 'http://127.0.0.1:8765';
	await page.goto(`${origin}/docs/fab/src/thumbnail.html`, { waitUntil: 'networkidle' });
	await page.evaluate(() => document.fonts.ready);
	const badge = page.locator('.badge b').filter({ hasText: /^\d+$/ });
	if (await badge.count() !== 1) throw new Error('Expected exactly one test-count badge');
	await badge.evaluate((element, value) => { element.textContent = value; }, count);
	await page.screenshot({ path: process.argv[2] || fileURLToPath(new URL('../site-card.png', import.meta.url)) });
	console.log(`Rendered site card with ${count} tests`);
} finally {
	await browser.close();
}
