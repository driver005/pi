// Generates src/testing/ts_golden/ts_evals_golden.json: random eval runs summarized and formatted by the TypeScript runner
// (packages/evals/src/report.ts), which the C++ EvalReportSummarizer and EvalReportFormatter must reproduce exactly. Usage (needs
// `npm install --ignore-scripts`):
//   esbuild tools/ts_evals_golden.mts --bundle --platform=node --format=esm --banner:js="import {createRequire as __cr} from 'module'; const require = __cr(import.meta.url);" --outfile=/tmp/evals_golden.mjs && node /tmp/evals_golden.mjs src/testing/ts_golden/ts_evals_golden.json
import { writeFileSync } from "node:fs";
import { type EvalTask } from "../packages/evals/src/plan.ts";
import { type EvalObservation, formatEvalComparisonReport, summarizeEvalObservations } from "../packages/evals/src/report.ts";

let seed = 12345;
function random(): number {
	seed = (seed * 1664525 + 1013904223) % 4294967296;
	return seed / 4294967296;
}
function pick<T>(values: readonly T[]): T {
	return values[Math.floor(random() * values.length)];
}

function scenario(index: number) {
	const sets = ["Add model", "Custom provider", "Extensions"].slice(0, 1 + (index % 3));
	const runs = 1 + (index % 4);
	const tasks: EvalTask[] = [];
	const observations: EvalObservation[] = [];
	const caseIds = ["adds the model", "handles errors"];
	for (const evalSet of sets) {
		for (const caseId of caseIds) {
			for (let run = 1; run <= runs; run += 1) {
				for (const variant of ["without_docs", "with_docs"] as const) {
					const task = { file: "evals/x.docs.eval.ts", fullName: `${evalSet} > ${caseId}`, evalSet, caseId, variant, model: "fixture/model", runNumber: run };
					tasks.push(task);
					const roll = random();
					if (index % 7 === 6 && roll < 0.1 && index % 3 !== 0) continue; // a missing observation
					const identity = { evalSet, caseId, variant, model: "fixture/model", runNumber: run };
					const metrics: Partial<EvalObservation> = {};
					if (random() < 0.85) metrics.totalTokens = Math.floor(random() * 4000);
					if (random() < 0.85) metrics.toolCalls = Math.floor(random() * 9);
					if (random() < 0.85) metrics.totalMs = Math.floor(random() * 90000) + (random() < 0.3 ? 0.25 : 0);
					if (random() < 0.85) metrics.estimatedCostUsd = Number((random() * 0.2).toFixed(pick([2, 3, 4, 5])));
					if (random() < 0.5) metrics.inputTokens = Math.floor(random() * 3000);
					if (random() < 0.5) metrics.outputTokens = Math.floor(random() * 800);
					if (random() < 0.4) metrics.cacheReadTokens = Math.floor(random() * 500);
					if (random() < 0.4) metrics.cacheWriteTokens = Math.floor(random() * 500);
					const clean = index % 3 === 0;
					const outcome = clean ? "scored" : roll < 0.08 ? "errored" : roll < 0.12 ? "skipped" : roll < 0.15 ? "unscored" : roll < 0.17 ? "pending" : "scored";
					if (outcome === "scored") {
						observations.push({ ...identity, ...metrics, outcome, score: pick([0, 0.5, 1, 1, 1]) });
					} else {
						observations.push({ ...identity, ...metrics, outcome } as EvalObservation);
					}
				}
			}
		}
	}
	const report = summarizeEvalObservations(`digest-${index}`, tasks, observations);
	const text = formatEvalComparisonReport(report).replace(/\u001b\[[0-9;]*m/g, "");
	return { digest: `digest-${index}`, tasks, observations, report, text };
}

const scenarios = Array.from({ length: 60 }, (_, index) => scenario(index));
const target = process.argv[2];
if (!target) throw new Error("usage: node evals_golden.mjs <output.json>");
writeFileSync(target, `${JSON.stringify({ scenarios })}\n`);
