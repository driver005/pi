// Regenerates src/testing/ts_golden/ts_theme_golden.json: custom themes (okhsl, oklch, palette indexes, variable references, terminal
// defaults, explicit and derived export colors) as the TypeScript HTML export resolves them. Needs `npm install --ignore-scripts`:
//   node_modules/.bin/esbuild tools/ts_theme_golden.mts --bundle --platform=node --format=esm --banner:js="import {createRequire as __cr} from 'module'; const require = __cr(import.meta.url);" --outfile=/tmp/theme_golden.mjs && PI_PACKAGE_DIR=packages/coding-agent node /tmp/theme_golden.mjs src/testing/ts_golden/ts_theme_golden.json
import { mkdirSync, mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const root = mkdtempSync(join(tmpdir(), "pi-theme-golden-"));
process.env.PI_CODING_AGENT_DIR = join(root, "agent");
mkdirSync(join(root, "agent", "themes"), { recursive: true });

const { exportFromFile } = await import("../packages/coding-agent/src/core/export-html/index.ts");
const { getResolvedThemeColors, isLightTheme } = await import("../packages/coding-agent/src/modes/interactive/theme/theme.ts");

const themeDir = "packages/coding-agent/src/modes/interactive/theme";
const dark = JSON.parse(readFileSync(join(themeDir, "dark.json"), "utf-8"));
const light = JSON.parse(readFileSync(join(themeDir, "light.json"), "utf-8"));
const clone = (value: unknown) => JSON.parse(JSON.stringify(value));

const themes: Array<Record<string, any>> = [];
const add = (name: string, build: (theme: Record<string, any>) => void, base = dark) => {
	const theme = clone(base);
	theme.name = name;
	build(theme);
	themes.push(theme);
};

add("derived-dark", (theme) => {
	delete theme.export;
});
add("derived-light", (theme) => {
	delete theme.export;
}, light);
add("explicit-export", (theme) => {
	theme.vars = { ...(theme.vars ?? {}), page: "#101820", card: "okhsl(250 40% 20%)", info: 52, alias: "page" };
	theme.export = { pageBg: "alias", cardBg: "card", infoBg: "info" };
});
add("oklch-export", (theme) => {
	theme.export = { pageBg: "oklch(25% 0.02 250)", cardBg: "oklch(0.3 0.03 250deg)", infoBg: "" };
});
add("mixed-spaces", (theme) => {
	theme.vars = { ...theme.vars, brand: "okhsl(145 70% 55%)", soft: "oklch(72% 0.09 30)", ink: "#ABC", base: 236, ref: "brand" };
	theme.colors.accent = "ref";
	theme.colors.mdLink = "soft";
	theme.colors.text = "ink";
	theme.colors.userMessageBg = "base";
	theme.colors.customMessageBg = "okhsl(280 30% 16%)";
	theme.colors.toolSuccessBg = "oklch(30% 0.05 150)";
	theme.colors.syntaxKeyword = 205;
	theme.colors.syntaxString = 4;
	theme.colors.border = "oklch(100% 0.3 150)";
	delete theme.export;
});
add("terminal-defaults", (theme) => {
	theme.colors.text = "";
	theme.colors.userMessageText = "";
	theme.colors.userMessageBg = "";
	theme.colors.toolPendingBg = "";
	delete theme.export;
});
add("declared-light", (theme) => {
	theme.appearance = "light";
	delete theme.export;
});
add("optional-tokens", (theme) => {
	theme.colors.scrollbarTrack = "#123456";
	theme.colors.searchMatchBg = "okhsl(60 90% 50%)";
	delete theme.colors.thinkingMax;
	delete theme.export;
});
add("palette-only-foregrounds", (theme) => {
	for (const key of Object.keys(theme.colors)) {
		if (/Bg$/.test(key)) theme.colors[key] = "";
		else theme.colors[key] = 3;
	}
	theme.vars = {};
	delete theme.export;
});

writeFileSync(
	join(root, "session.jsonl"),
	`${JSON.stringify({ type: "session", version: 3, id: "s1", timestamp: "2025-01-01T00:00:00.000Z", cwd: "/tmp" })}\n${JSON.stringify({ type: "message", id: "e1", parentId: null, timestamp: "2025-01-01T00:00:01.000Z", message: { role: "user", content: "hi", timestamp: 1 } })}\n`,
);

const out: unknown[] = [];
for (const theme of themes) {
	writeFileSync(join(root, "agent", "themes", `${theme.name}.json`), JSON.stringify(theme));
	const outputPath = join(root, `${theme.name}.html`);
	await exportFromFile(join(root, "session.jsonl"), { outputPath, themeName: theme.name });
	const html = readFileSync(outputPath, "utf-8");
	const pick = (variable: string) => new RegExp(`--${variable}: ([^;]*);`).exec(html)?.[1];
	out.push({
		theme,
		appearance: isLightTheme(theme.name) ? "light" : "dark",
		colors: getResolvedThemeColors(theme.name),
		export: { pageBg: pick("exportPageBg"), cardBg: pick("exportCardBg"), infoBg: pick("exportInfoBg") },
	});
}
const target = process.argv[2];
if (!target) throw new Error("usage: node theme_golden.mjs <output.json>");
writeFileSync(target, `${JSON.stringify(out, null, 1)}\n`);
