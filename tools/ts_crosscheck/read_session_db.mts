import { BACKGROUND_CONTEXT } from "@earendil-works/chord/context";
import { openNodeSqliteStorage } from "@earendil-works/pi-durable/storage/sqlite/node";
const path = process.argv[2];
const storage = await openNodeSqliteStorage(path);
const ctx = BACKGROUND_CONTEXT;
const convs = await storage.scanConversations({} as any, 100, undefined, ctx);
console.log("conversations", JSON.stringify(convs.items));
for (const c of convs.items as any[]) {
	const entries = await storage.scanEntries({ conversationId: c.id }, 100, undefined, ctx);
	console.log("entries", c.id, entries.items.length);
	for (const e of entries.items as any[]) console.log(" ", e.id, JSON.stringify(e.value ?? e).slice(0, 160));
	const tasks = await storage.scanTasks({ conversationId: c.id }, 100, undefined, ctx);
	console.log("tasks", tasks.items.map((t: any) => `${t.id}:${t.kind}:${t.status?.type ?? t.status}`).join(","));
}
await storage.close();
