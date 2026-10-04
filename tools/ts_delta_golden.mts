// Regenerates src/testing/ts_golden/ts_delta_golden.json from the TypeScript Delta code of packages/chord:
//   node --experimental-strip-types tools/ts_delta_golden.mts src/testing/ts_golden/ts_delta_golden.json
import { writeFileSync } from "node:fs";
import { apply, assertValidOp, assertValidWireOp, decoder, diffRevisions, encoder } from "../packages/chord/src/delta/index.ts";

type Json = null | boolean | number | string | Json[] | { [key: string]: Json };

const clone = (value: unknown): Json => JSON.parse(JSON.stringify(value)) as Json;
const message = (error: unknown): string => (error instanceof Error ? error.message : String(error));

const attempt = <T>(run: () => T): { accepted: true; value: T } | { accepted: false; error: string } => {
	try {
		return { accepted: true, value: run() };
	} catch (error) {
		return { accepted: false, error: message(error) };
	}
};

// Deterministic generator so regeneration is stable.
let seed = 20240607;
const random = (): number => {
	seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
	return seed / 4294967296;
};
const pick = (limit: number): number => Math.floor(random() * limit);

// Lone surrogates have no UTF-8 form, so no vector may produce one.
const words = ["a", "bc", "def", "x y", "line\n", "été", "😀", "tab\t", "q\"uote", "", "same"];
const keys = ["alpha", "beta", "gamma", "delta", "k0", "k1", "k2", "text", "list"];

const randomValue = (depth: number): Json => {
	const kind = pick(depth <= 0 ? 4 : 7);
	switch (kind) {
		case 0:
			return pick(5) === 0 ? null : pick(2) === 0;
		case 1:
			return pick(2000) - 1000;
		case 2:
		case 3:
			return words[pick(words.length)]! + (pick(3) === 0 ? words[pick(words.length)]! : "");
		case 4:
		case 5: {
			const items: Json[] = [];
			for (let i = pick(6); i > 0; i--) items.push(randomValue(depth - 1));
			return items;
		}
		default: {
			const entries: { [key: string]: Json } = {};
			for (let i = pick(5); i > 0; i--) entries[keys[pick(keys.length)]!] = randomValue(depth - 1);
			return entries;
		}
	}
};

const mutate = (value: Json, depth: number): Json => {
	if (pick(6) === 0) return randomValue(depth);
	if (Array.isArray(value)) {
		const next = value.slice();
		switch (pick(5)) {
			case 0:
				next.splice(pick(next.length + 1), 0, randomValue(depth - 1));
				break;
			case 1:
				next.splice(pick(next.length + 1), pick(3));
				break;
			case 2:
				for (let i = next.length - 1; i > 0; i--) {
					const j = pick(i + 1);
					[next[i], next[j]] = [next[j]!, next[i]!];
				}
				break;
			default:
				if (next.length > 0) {
					const index = pick(next.length);
					next[index] = mutate(next[index]!, depth - 1);
				}
		}
		return next;
	}
	if (value !== null && typeof value === "object") {
		const next = { ...value };
		const existing = Object.keys(next);
		switch (pick(4)) {
			case 0:
				next[keys[pick(keys.length)]!] = randomValue(depth - 1);
				break;
			case 1:
				if (existing.length > 0) delete next[existing[pick(existing.length)]!];
				break;
			default:
				if (existing.length > 0) {
					const key = existing[pick(existing.length)]!;
					next[key] = mutate(next[key]!, depth - 1);
				}
		}
		return next;
	}
	if (typeof value === "string") {
		switch (pick(3)) {
			case 0:
				return value + words[pick(words.length)]!;
			case 1:
				return Array.from(value).slice(pick(Array.from(value).length + 1)).join("");
			default:
				return words[pick(words.length)]!;
		}
	}
	return randomValue(0);
};

// ─── diff vectors, which double as apply and encode vectors ───────────────────

const revisions: Array<{ before: Json; after: Json }> = [];
const diffs = [] as Array<{ name: string; before: Json; after: Json; ops: Json }>;
for (let index = 0; index < 200; index++) {
	const before: Json = pick(4) === 0 ? randomValue(3) : { text: words[pick(words.length)]!, list: randomValue(2), nested: randomValue(2) };
	let after = clone(before);
	for (let steps = 1 + pick(3); steps > 0; steps--) after = mutate(after, 3);
	revisions.push({ before, after });
	diffs.push({ name: `fuzz ${index}`, before, after, ops: clone(diffRevisions(clone(before), clone(after))) });
}

// ─── apply ────────────────────────────────────────────────────────────────────

const applyCases: Array<{ name: string; target: Json; ops: Json }> = [
	{ name: "set", target: { a: 1 }, ops: [["s", ["a"], 2]] },
	{ name: "set new key", target: { a: 1 }, ops: [["s", ["b"], 2]] },
	{ name: "set nested", target: { a: { b: 1 } }, ops: [["s", ["a", "b"], { c: [1, 2] }]] },
	{ name: "set array element", target: { v: [1, 2, 3] }, ops: [["s", ["v", 1], 9]] },
	{ name: "set append one past end", target: { v: [1, 2] }, ops: [["s", ["v", 2], 9]] },
	{ name: "set past end", target: { v: [1, 2] }, ops: [["s", ["v", 3], 9]] },
	{ name: "set through missing", target: { a: 1 }, ops: [["s", ["x", "y"], 9]] },
	{ name: "set through scalar", target: { a: 1 }, ops: [["s", ["a", "y"], 9]] },
	{ name: "delete key", target: { a: 1, b: 2 }, ops: [["d", ["a"]]] },
	{ name: "delete missing key", target: { a: 1 }, ops: [["d", ["z"]]] },
	{ name: "delete array element", target: { v: [1, 2, 3] }, ops: [["d", ["v", 1]]] },
	{ name: "delete past end", target: { v: [1] }, ops: [["d", ["v", 1]]] },
	{ name: "append", target: { s: "ab" }, ops: [["a", ["s"], "cd"]] },
	{ name: "append to non string", target: { s: 1 }, ops: [["a", ["s"], "cd"]] },
	{ name: "append to missing", target: { s: "a" }, ops: [["a", ["t"], "cd"]] },
	{ name: "truncate", target: { s: "abcd" }, ops: [["t", ["s"], 2]] },
	{ name: "truncate beyond", target: { s: "ab" }, ops: [["t", ["s"], 9]] },
	{ name: "truncate counts utf16 units", target: { s: "😀x" }, ops: [["t", ["s"], 2]] },
	{ name: "truncate non string", target: { s: 1 }, ops: [["t", ["s"], 1]] },
	{ name: "splice insert", target: { v: [1, 2, 3] }, ops: [["p", ["v"], 1, 0, [9, 8]]] },
	{ name: "splice replace", target: { v: [1, 2, 3] }, ops: [["p", ["v"], 1, 1, [9]]] },
	{ name: "splice remove", target: { v: [1, 2, 3] }, ops: [["p", ["v"], 0, 2, []]] },
	{ name: "splice beyond", target: { v: [1, 2, 3] }, ops: [["p", ["v"], 5, 5, [7]]] },
	{ name: "splice on object", target: { v: { a: 1 } }, ops: [["p", ["v"], 0, 0, [1]]] },
	{ name: "root splice", target: [1, 2, 3], ops: [["p", [], 1, 1, [4, 5]]] },
	{ name: "permute", target: { v: ["a", "b", "c"] }, ops: [["m", ["v"], [2, 0, 1]]] },
	{ name: "permute wrong length", target: { v: ["a", "b", "c"] }, ops: [["m", ["v"], [1, 0]]] },
	{ name: "permute not bijection", target: { v: ["a", "b"] }, ops: [["m", ["v"], [0, 0]]] },
	{ name: "root permute", target: [1, 2, 3], ops: [["m", [], [2, 1, 0]]] },
	{ name: "replace root", target: { a: 1 }, ops: [["r", [1, 2]]] },
	{ name: "replace from null", target: null, ops: [["r", { a: 1 }]] },
	{ name: "replace then edit", target: 5, ops: [["r", { a: "x" }], ["a", ["a"], "y"]] },
	{ name: "mixed", target: { text: "a", values: [1, 2], nested: { value: 1 } }, ops: [["a", ["text"], "b"], ["p", ["values"], 1, 1, [3, 4]], ["s", ["nested", "value"], 2]] },
	{ name: "key order after delete and set", target: { a: 1, b: 2, c: 3 }, ops: [["d", ["a"]], ["s", ["a"], 9]] },
	{ name: "reserved set", target: { a: 1 }, ops: [["s", ["__proto__", "x"], 1]] },
	{ name: "reserved constructor", target: { a: 1 }, ops: [["s", ["constructor"], 1]] },
	{ name: "reserved prototype", target: { a: { b: 1 } }, ops: [["d", ["a", "prototype"]]] },
	{ name: "negative index", target: { v: [1] }, ops: [["s", ["v", -1], 1]] },
	{ name: "fractional index", target: { v: [1] }, ops: [["s", ["v", 0.5], 1]] },
	{ name: "string index on array", target: { v: [1, 2] }, ops: [["s", ["v", "0"], 1]] },
	{ name: "empty path set", target: { a: 1 }, ops: [["s", [], 1]] },
	{ name: "unknown verb", target: { a: 1 }, ops: [["z", ["a"]]] },
	{ name: "second op fails", target: { a: 1 }, ops: [["s", ["a"], 2], ["a", ["a"], "x"]] },
	{ name: "empty batch", target: { a: 1 }, ops: [] },
];
for (const { name, before, ops } of diffs.map((d) => ({ name: d.name, before: d.before, ops: d.ops }))) {
	applyCases.push({ name: `apply ${name}`, target: before, ops });
}
const applyVectors = applyCases.map(({ name, target, ops }) => {
	const outcome = attempt(() => clone(apply(clone(target), clone(ops) as never)));
	return outcome.accepted ? { name, target, ops, accepted: true, result: outcome.value } : { name, target, ops, accepted: false, error: outcome.error };
});

// ─── validation ───────────────────────────────────────────────────────────────

const opCandidates: Json[] = [
	["r", 1],
	["r"],
	["r", 1, 2],
	["s", ["a"], 1],
	["s", [], 1],
	["s", ["a"]],
	["s", 1],
	["s", "a", 1],
	["s", ["a", 0, "b"], null],
	["s", ["a", -1], 1],
	["s", ["a", 1.5], 1],
	["s", ["a", true], 1],
	["s", ["a", null], 1],
	["s", ["__proto__"], 1],
	["s", ["x", "constructor"], 1],
	["s", ["prototype"], 1],
	["d", ["a"]],
	["d", []],
	["d"],
	["d", 0],
	["d", ["a"], 1],
	["a", ["a"], "x"],
	["a", ["a"], 1],
	["a", "x"],
	["a", 0, "x"],
	["a", ["a"]],
	["t", ["a"], 1],
	["t", ["a"], -1],
	["t", ["a"], 1.5],
	["t", ["a"], "1"],
	["t", 2],
	["t", 0, 2],
	["t", ["a"], 0],
	["p", ["a"], 0, 0, []],
	["p", [], 0, 0, [1]],
	["p", ["a"], -1, 0, []],
	["p", ["a"], 0, -1, []],
	["p", ["a"], 0, 0, "x"],
	["p", ["a"], 0, 0],
	["p", 0, 1, [2]],
	["p", 0, 0, 1, [2]],
	["p", 1, 1, "x"],
	["m", ["a"], [1, 0]],
	["m", [], [0]],
	["m", ["a"], [0, 0]],
	["m", ["a"], [0, 2]],
	["m", ["a"], [-1]],
	["m", ["a"], [0.5]],
	["m", ["a"], "x"],
	["m", [1, 0]],
	["m", 3, [1, 0]],
	["m", [1, 1]],
	["#", 0, ["a"]],
	["#", 0, []],
	["#", -1, ["a"]],
	["#", 0.5, ["a"]],
	["#", 0, "a"],
	["#", 0, ["__proto__"]],
	["#", 0],
	["#"],
	["x", ["a"]],
	["x"],
	[],
	"s",
	null,
	{},
	[1, ["a"], 1],
];
const validations = opCandidates.map((op) => {
	const decoded = attempt(() => assertValidOp(op));
	const wire = attempt(() => assertValidWireOp(op));
	return { op, op_accepted: decoded.accepted, wire_accepted: wire.accepted };
});

// ─── encode: one encoder per scenario, batches in order ──────────────────────

const encodeScenarios: Array<{ name: string; batches: Json[] }> = [
	{ name: "second use interns", batches: [[["a", ["a", "deep"], "1"]], [["a", ["a", "deep"], "2"]], [["a", ["a", "deep"], "3"]]] },
	{ name: "adjacent paths drop", batches: [[["s", ["value"], 1], ["s", ["value"], 2], ["s", ["value"], 3]]] },
	{ name: "every verb shortens", batches: [[["d", ["a"]], ["d", ["a"]], ["t", ["b"], 1], ["t", ["b"], 2], ["p", ["c"], 0, 0, [1]], ["p", ["c"], 1, 0, [2]], ["m", ["e"], [1, 0]], ["m", ["e"], [0, 1]]]] },
	{ name: "base resets dictionary", batches: [[["s", ["v"], 1]], [["s", ["v"], 2]], [["s", ["v"], 3]], [["r", { v: 3 }]], [["s", ["v"], 4]], [["s", ["v"], 5]]] },
	{ name: "omission is per batch", batches: [[["s", ["a"], 1]], [["s", ["a"], 2]], [["s", ["b"], 3], ["s", ["b"], 4]]] },
	{ name: "interleaved paths", batches: [[["s", ["a"], 1], ["s", ["b"], 2], ["s", ["a"], 3], ["s", ["b"], 4], ["s", ["a"], 5], ["s", ["b"], 6]]] },
	{ name: "root paths", batches: [[["p", [], 0, 0, [1]], ["p", [], 1, 0, [2]]], [["m", [], [1, 0]]], [["m", [], [0, 1]]]] },
	{ name: "colliding keys", batches: [[["s", ["a\u0000b"], 1], ["s", ["a", "b"], 2]], [["s", ["a\u0000b"], 3], ["s", ["a", "b"], 4]]] },
	{ name: "integer and string segments", batches: [[["s", ["v", 0], 1]], [["s", ["v", 0], 2]], [["s", ["v", "0"], 3]], [["s", ["v", "0"], 4]]] },
	{ name: "empty batch", batches: [[], [["s", ["a"], 1]], []] },
];
for (let start = 0; start < diffs.length; start += 8) {
	encodeScenarios.push({ name: `diff stream ${start / 8}`, batches: diffs.slice(start, start + 8).map((d) => d.ops) });
}
const encodeVectors = encodeScenarios.map(({ name, batches }) => {
	const codec = encoder();
	return { name, batches, wire: batches.map((batch) => clone(codec.encode(clone(batch) as never))) };
});

// ─── decode: one decoder per scenario, stop at the first rejected batch ───────

const wireScenarios: Array<{ name: string; batches: Json[] }> = [
	{ name: "define then reference", batches: [[["#", 0, ["a", "b"]], ["s", 0, 1], ["s", 2]]] },
	{ name: "definition spans batches", batches: [[["#", 0, ["a"]], ["s", 0, 1]], [["s", 0, 2]]] },
	{ name: "short form needs previous", batches: [[["a", "x"]]] },
	{ name: "short form across batches", batches: [[["a", ["p"], "x"]], [["a", "y"]]] },
	{ name: "every verb short", batches: [[["d", ["a"]], ["d"], ["t", ["b"], 1], ["t", 2], ["p", ["c"], 0, 0, [1]], ["p", 1, 0, [2]], ["m", ["e"], [1, 0]], ["m", [0, 1]]]] },
	{ name: "base clears ids", batches: [[["#", 0, ["a"]], ["a", 0, "1"]], [["r", { a: "" }]], [["a", 0, "2"]]] },
	{ name: "base clears previous", batches: [[["a", ["p"], "x"], ["r", { p: "" }], ["a", "y"]]] },
	{ name: "unresolved id", batches: [[["s", 7, 1]]] },
	{ name: "redefine id", batches: [[["#", 0, ["a"]], ["#", 0, ["b"]], ["s", 0, 1]]] },
	{ name: "unsafe definition", batches: [[["#", 0, ["__proto__"]], ["s", 0, true]]] },
	{ name: "unsafe inline", batches: [[["s", ["__proto__", "w"], true]]] },
	{ name: "root target for s", batches: [[["#", 0, []], ["s", 0, 1]]] },
	{ name: "root target for p", batches: [[["#", 0, []], ["p", 0, 0, 0, [1]]]] },
	{ name: "root target for m", batches: [[["#", 0, []], ["m", 0, [0]]]] },
	{ name: "root target for t", batches: [[["#", 0, []], ["t", 0, 1]]] },
	{ name: "negative count", batches: [[["t", ["value"], -1]]] },
	{ name: "bad permutation", batches: [[["m", ["v"], [0, 0]]]] },
	{ name: "unknown verb", batches: [[["z", ["a"]]]] },
	{ name: "empty batch", batches: [[], [["s", ["a"], 1]]] },
	{ name: "a with number value", batches: [[["a", ["a"], 1]]] },
	{ name: "t short with string", batches: [[["t", ["a"], 1], ["t", "x"]]] },
	{ name: "s value that looks like a path", batches: [[["s", ["a"], ["b"]], ["s", ["c"]]]] },
];
for (const scenario of encodeVectors.slice(0, 10)) {
	wireScenarios.push({ name: `encoded ${scenario.name}`, batches: scenario.wire });
}
for (const scenario of encodeVectors.slice(10, 30)) {
	wireScenarios.push({ name: `encoded ${scenario.name}`, batches: scenario.wire });
}
const decodeVectors = wireScenarios.map(({ name, batches }) => {
	const codec = decoder();
	const results: Array<{ accepted: boolean; ops?: Json; error?: string }> = [];
	for (const batch of batches) {
		const outcome = attempt(() => clone(codec.decode(clone(batch) as never)));
		if (outcome.accepted) results.push({ accepted: true, ops: outcome.value });
		else {
			results.push({ accepted: false, error: outcome.error });
			break;
		}
	}
	return { name, batches, results };
});

// Large revisions exercise the operation cap and the snapshot-versus-delta cost rule.
const bigList = (count: number, tag: string): Json[] => Array.from({ length: count }, (_, i) => `${tag}-${i}`);
const manyKeys = (tag: string): { [key: string]: Json } => Object.fromEntries(Array.from({ length: 5000 }, (_, i) => [`k${i}`, `${tag}${i}`]));
for (const [name, before, after] of [
	["more than 4096 operations", { v: manyKeys("a") }, { v: manyKeys("b") }],
	["large rewrite becomes snapshot", { v: bigList(4000, "old") }, { v: bigList(4000, "new") }],
	["large but narrow delta stays", { v: bigList(4000, "old"), w: 1 }, { v: bigList(4000, "old"), w: 2 }],
	["large splice stays", { v: bigList(4000, "old") }, { v: [...bigList(2000, "old"), "inserted", ...bigList(2000, "old").map((x) => `${x}!`)] }],
] as Array<[string, Json, Json]>) {
	diffs.push({ name, before, after, ops: clone(diffRevisions(clone(before), clone(after))) });
}

const diffVectors = diffs.map(({ name, before, after, ops }) => ({ name, before, after, ops }));

const output = process.argv[2];
if (output === undefined) throw new Error("usage: ts_delta_golden.mts <output.json>");
writeFileSync(
	output,
	`${JSON.stringify({ apply: applyVectors, validate: validations, encode: encodeVectors, decode: decodeVectors, diff: diffVectors })}\n`,
);
console.log(
	`apply ${applyVectors.length} (${applyVectors.filter((v) => v.accepted).length} accepted), validate ${validations.length}, encode ${encodeVectors.length}, decode ${decodeVectors.length}, diff ${diffVectors.length}`,
);
