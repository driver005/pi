// Writes the resolved colors of the built-in themes (dark, light) as the HTML export needs them: every color token as a CSS color
// and the explicit export colors of the theme. The C++ HTML export embeds the result (assets/export_html/export_themes.json), so it
// needs no color-space code of its own. Usage (needs `npm install --ignore-scripts`):
//   esbuild tools/ts_export_themes.mts --bundle --platform=node --format=esm --banner:js="import {createRequire as __cr} from 'module'; const require = __cr(import.meta.url);" --outfile=/tmp/themes.mjs && PI_PACKAGE_DIR=packages/coding-agent node /tmp/themes.mjs assets/export_html/export_themes.json
import { writeFileSync } from "node:fs";
import { getResolvedThemeColors, getThemeExportColors, isLightTheme } from "../packages/coding-agent/src/modes/interactive/theme/theme.ts";

const out: Record<string, unknown> = {};
for (const name of ["dark", "light"]) {
	out[name] = { appearance: isLightTheme(name) ? "light" : "dark", colors: getResolvedThemeColors(name), export: getThemeExportColors(name) };
}
const target = process.argv[2];
if (!target) throw new Error("usage: node themes.mjs <output.json>");
writeFileSync(target, `${JSON.stringify(out, null, 2)}\n`);
