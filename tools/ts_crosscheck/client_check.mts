import { spawn } from "node:child_process";
import { mkdtempSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { Client } from "@earendil-works/pi-client";
import { createUnixTransportFactory } from "@earendil-works/pi-client/unix";

const backend = process.argv[2] ?? "durable";
const PI = process.env.PI_BINARY ?? "bazel-bin/app/pi/pi";
const SERVER_ID = "01234567-89ab-4def-8123-456789abcdef";
const root = mkdtempSync(join(tmpdir(), "xcheck-"));
const args = ["serve", "--faux", "--server-dir", join(root, "srv"), "--server-id", SERVER_ID, "--agent-dir", join(root, "agent")];
if (backend === "tree") args.push("--session-tree");
const child = spawn(PI, args, {
	cwd: root,
	env: { ...process.env, PI_FAUX_REPLIES: JSON.stringify(["pong from faux"]), HOME: root },
	stdio: ["ignore", "inherit", "inherit"],
});
const sock = join(root, "srv", `${SERVER_ID}.sock`);
for (let i = 0; i < 200 && !existsSync(sock); i++) await new Promise((r) => setTimeout(r, 25));
const fail = (m: string): never => { console.error("FAIL:", m); child.kill(); process.exit(1); };
const call = (serviceId: string, member: string, a: unknown[]) => ({ serviceId, member, args: a }) as any;
try {
	const client = new Client({
		serverId: SERVER_ID,
		transportFactory: createUnixTransportFactory({ path: sock }),
		onListenerError: (e) => console.error("listener error", e),
	});
	const hello = await client.connect();
	console.log("hello", JSON.stringify(hello));
	const cat = await client.serviceCatalogue({ serverId: SERVER_ID });
	console.log("catalogue", cat.map((e: any) => e.id ?? JSON.stringify(e)).join(","));
	const dir = await client.subscribeService({ serverId: SERVER_ID }, "pi.session-directory", "singleton", (u) => console.log("dir update", JSON.stringify(u)));
	console.log("dir snapshot", JSON.stringify(dir.snapshot));
	dir.start();
	const created = await client.request({ serverId: SERVER_ID }, call("pi.session-management", "create", [{ id: "demo" }]));
	console.log("created", JSON.stringify(created));
	await client.request({ serverId: SERVER_ID }, call("pi.session-management", "attach", ["demo"]));
	for (let i = 0; i < 200 && !client.attachment; i++) await new Promise((r) => setTimeout(r, 25));
	const target = client.attachment ?? fail("no attachment");
	console.log("attachment", JSON.stringify(target));
	let latest = "";
	const sub = await client.subscribeService(target, "pi.transcript", "singleton", (u) => { latest += JSON.stringify(u); });
	console.log("transcript snapshot", JSON.stringify(sub.snapshot).slice(0, 400));
	sub.start();
	const prompted: any = await client.request(target, call("pi.agent-controller", "prompt", [{ message: "ping", images: null }]));
	console.log("prompted", JSON.stringify(prompted));
	const answer: any = await client.request(target, call("pi.agent-controller", "waitForPrompt", [prompted.operationId]));
	console.log("answer", JSON.stringify(answer));
	if (answer.text !== "pong from faux") fail("unexpected answer");
	for (let i = 0; i < 200 && !latest.includes("pong from faux"); i++) await new Promise((r) => setTimeout(r, 25));
	if (!latest.includes("pong from faux")) fail("transcript never streamed the reply");
	console.log("transcript updates carried reply, bytes:", latest.length);
	const sessCat = await client.serviceCatalogue(target);
	console.log("session catalogue", sessCat.map((e: any) => e.serviceId + ":" + e.mode).join(","));
	for (const e of sessCat as any[]) {
		if (e.serviceId === "pi.transcript") continue;
		const s2 = await client.subscribeService(target, e.serviceId, e.mode, (u) => console.log("  update", e.serviceId, JSON.stringify(u).slice(0, 200)));
		console.log("subscribed", e.serviceId, JSON.stringify(s2.snapshot).slice(0, 300));
		s2.start();
		await s2.dispose();
	}
	// cancel: abort a long wait
	const ac = new AbortController();
	const p2: any = await client.request(target, call("pi.agent-controller", "prompt", [{ message: "second", images: null }]));
	console.log("second prompt", JSON.stringify(p2));
	if (p2.accepted) {
		const w = client.request(target, call("pi.agent-controller", "waitForPrompt", [p2.operationId]), ac.signal);
		setTimeout(() => ac.abort(), 20);
		try { console.log("wait2", JSON.stringify(await w)); } catch (e) { console.log("wait2 aborted:", String(e)); }
	}
	await sub.dispose();
	await dir.dispose();
	await client.dispose();
	if (backend === "durable") {
		child.kill();
		await new Promise((r) => child.once("exit", r));
		const child2 = spawn(PI, args, { cwd: root, env: { ...process.env, PI_FAUX_REPLIES: "[]", HOME: root }, stdio: ["ignore", "inherit", "inherit"] });
		await new Promise((r) => setTimeout(r, 500));
		const c2 = new Client({ serverId: SERVER_ID, transportFactory: createUnixTransportFactory({ path: sock }) });
		await c2.connect();
		const dir2: any = await c2.subscribeService({ serverId: SERVER_ID }, "pi.session-directory", "singleton", () => {});
		console.log("after restart directory", JSON.stringify(dir2.snapshot).slice(0, 300));
		await c2.request({ serverId: SERVER_ID }, call("pi.session-management", "attach", ["demo"]));
		for (let i = 0; i < 200 && !c2.attachment; i++) await new Promise((r) => setTimeout(r, 25));
		const t2 = c2.attachment ?? fail("no reattachment");
		const s3: any = await c2.subscribeService(t2, "pi.transcript", "singleton", () => {});
		const text = JSON.stringify(s3.snapshot);
		if (!text.includes("pong from faux")) fail("restarted transcript lacks reply: " + text.slice(0, 400));
		console.log("restart transcript retained reply");
		await c2.dispose();
		child2.kill();
	}
	console.log("DB", join(root, "agent", "server-sessions", "demo", "session.sqlite"));
	console.log("OK", backend);
} catch (e) {
	fail(String((e as Error)?.stack ?? e));
}
child.kill();
