// Browser E2E runner: node browser_test.mjs <url>
// Loads the page, waits for window.__jelll_done, prints {results, errors} as JSON.
import { chromium } from "playwright";

const url = process.argv[2];
if (!url) {
  console.error("usage: node browser_test.mjs <url>");
  process.exit(2);
}

const browser = await chromium.launch();
const page = await browser.newPage();
const errors = [];
page.on("pageerror", (e) => errors.push("pageerror: " + e.message));
page.on("console", (m) => {
  if (m.type() === "error") errors.push("console: " + m.text());
});

try {
  await page.goto(url, { waitUntil: "domcontentloaded", timeout: 20000 });
  await page.waitForFunction("window.__jelll_done === true", null, { timeout: 20000 });
  const results = await page.evaluate("window.__jelll_results");
  console.log(JSON.stringify({ results, errors }));
} catch (e) {
  console.log(JSON.stringify({ results: null, errors: [...errors, "harness: " + e.message] }));
  await browser.close();
  process.exit(3);
}
await browser.close();
if (errors.length) process.exit(3);
