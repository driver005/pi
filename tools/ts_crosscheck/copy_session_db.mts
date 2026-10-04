import { BACKGROUND_CONTEXT } from "@earendil-works/chord/context";
import { openNodeSqliteStorage } from "@earendil-works/pi-durable/storage/sqlite/node";
const [src, dst] = process.argv.slice(2);
const from = await openNodeSqliteStorage(src);
const ctx = BACKGROUND_CONTEXT;
const convs = await from.scanConversations({} as any, 100, undefined, ctx);
const writes: any[] = [];
for (const c of convs.items as any[]) {
	writes.push({ type: "conversation", value: c });
	const entries = await from.scanEntries({ conversationId: c.id }, 100, undefined, ctx);
	for (const e of [...(entries.items as any[])].sort((a, b) => a.id - b.id)) writes.push({ type: "entry", value: e });
}
const scopes: any[] = [{ kind: "session" }, ...(convs.items as any[]).map((c) => ({ kind: "conversation", conversationId: c.id }))];
for (const scope of scopes) {
	const docs = await from.scanDocuments({ scope, at: "current" } as any, 100, undefined, ctx);
	for (const d of docs.items as any[]) {
		const stored: any = await from.document(d.id, "current", ctx);
		const { createdAt: _c, retiredAt: _r, ...record } = d;
		writes.push({ type: "document.create", record, content: { kind: "base", version: stored.version, value: stored.value } });
	}
}
console.log("docs", writes.filter((w) => w.type === "document.create").map((w) => w.record.kind).join(","));
await from.close();
const to = await openNodeSqliteStorage(dst);
await to.commit(writes, ctx);
await to.close();
console.log("wrote", writes.length, "records");
