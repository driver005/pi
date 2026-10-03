// Regenerates src/testing/ts_golden/ts_cbor_golden.json from the TypeScript CBOR and framing code of packages/protocol:
//   node --experimental-strip-types tools/ts_cbor_golden.mts src/testing/ts_golden/ts_cbor_golden.json
import { writeFileSync } from "node:fs";
import { decodeCbor, encodeCbor } from "../packages/protocol/src/cbor/index.ts";
import { encodeFrame } from "../packages/protocol/src/framing.ts";

const hex = (bytes: Uint8Array): string => Buffer.from(bytes).toString("hex");
const fromHex = (text: string): Uint8Array => new Uint8Array(Buffer.from(text, "hex"));

const deep = (levels: number): unknown => {
	let value: unknown = 1;
	for (let i = 0; i < levels; i++) value = [value];
	return value;
};

const encodeValues: Array<{ name: string; json: string }> = [
	["null", "null"],
	["true", "true"],
	["false", "false"],
	["zero", "0"],
	["small", "23"],
	["one byte", "24"],
	["byte max", "255"],
	["two bytes", "256"],
	["two bytes max", "65535"],
	["four bytes", "65536"],
	["four bytes max", "4294967295"],
	["eight bytes", "4294967296"],
	["max safe", "9007199254740991"],
	["negative one", "-1"],
	["negative 24", "-24"],
	["negative 25", "-25"],
	["negative 256", "-256"],
	["negative 257", "-257"],
	["negative 65537", "-65537"],
	["negative four bytes", "-4294967297"],
	["min safe", "-9007199254740991"],
	["float half", "0.5"],
	["float", "1.5"],
	["float negative", "-2.25"],
	["float tiny", "1e-300"],
	["float large", "1.7976931348623157e+308"],
	["float pi", "3.141592653589793"],
	["float unsafe integer", "1e21"],
	["deep 65", JSON.stringify(deep(65))],
	["unsafe integer", "9007199254740992"],
	["empty string", '""'],
	["short string", '"abc"'],
	["24 byte string", JSON.stringify("x".repeat(24))],
	["300 byte string", JSON.stringify("y".repeat(300))],
	["unicode", '"h\\u00e9llo \\u4e16\\u754c \\ud83d\\ude00"'],
	["control characters", '"a\\u0000b\\n\\t\\u001f"'],
	["empty array", "[]"],
	["array", "[1,-1,0.5,null,true,\"s\"]"],
	["array of 24", JSON.stringify(Array.from({ length: 24 }, (_, i) => i))],
	["nested arrays", "[[],[[]],[1,[2,[3]]]]"],
	["empty object", "{}"],
	["object", '{"a":1,"b":[true,false],"c":{"d":null}}'],
	["object key order", '{"z":1,"a":2,"m":3}'],
	["unicode keys", '{"k\\u00e9y":1,"\\u4e16":2}'],
	["deep 64", JSON.stringify(deep(64))],
	[
		"client hello",
		'{"type":"hello","version":8}',
	],
	[
		"request",
		'{"type":"request","id":"r1","target":{"serverId":"00000000-0000-4000-8000-000000000001"},"call":{"serviceId":"pi.agent-controller","member":"prompt","args":[{"message":"ping","images":null}]}}',
	],
	[
		"response ok",
		'{"type":"response","id":"r1","ok":true,"result":{"accepted":true,"operationId":"7","error":null}}',
	],
	[
		"response error",
		'{"type":"response","id":"r2","ok":false,"error":{"code":"busy","message":"A run is already active"}}',
	],
	[
		"service update",
		'{"type":"service_update","subscriptionId":"s1","update":{"sequence":3,"ops":[["a",["entries",0,"text"],"abc"],["t",["o"],2],["p",["entries"],0,1,[{"id":1}]],["s",["x"],{"y":[1,2]}],["d",["z"]],["r",{"k":1}]]}}',
	],
].map(([name, json]) => ({ name, json }));

const rejectOrAccept: Array<{ name: string; hex: string }> = [
	["canonical small", "05"],
	["non-canonical one byte", "1805"],
	["non-canonical two bytes", "190005"],
	["non-canonical four bytes", "1a00000005"],
	["non-canonical eight bytes", "1b0000000000000005"],
	["max safe", "1b001fffffffffffff"],
	["unsafe integer", "1b0020000000000000"],
	["negative safe min", "3b001ffffffffffffe"],
	["negative unsafe", "3b001fffffffffffff"],
	["float64 one", "fb3ff0000000000000"],
	["float64 half", "fb3fe0000000000000"],
	["float64 negative zero", "fb8000000000000000"],
	["float64 infinity", "fb7ff0000000000000"],
	["float64 nan", "fb7ff8000000000000"],
	["float64 unsafe integer", "fb4340000000000000"],
	["float16", "f93c00"],
	["float32", "fa3fc00000"],
	["undefined", "f7"],
	["simple 16", "f0"],
	["simple value one byte", "f820"],
	["break", "ff"],
	["byte string", "420102"],
	["tag", "c000"],
	["tag 24", "d81800"],
	["indefinite text", "7f6161ff"],
	["indefinite array", "9fff"],
	["indefinite map", "bfff"],
	["empty text", "60"],
	["text", "63616263"],
	["invalid utf-8", "61ff"],
	["overlong utf-8", "62c080"],
	["surrogate utf-8", "63eda080"],
	["array", "83010203"],
	["map", "a2616101616202"],
	["duplicate keys", "a2616101616102"],
	["integer key", "a10101"],
	["byte string key", "a1410101"],
	["truncated array", "8301"],
	["truncated text", "6361"],
	["truncated argument", "19"],
	["trailing data", "0000"],
	["empty input", ""],
	["reserved additional information", "1c"],
	["nested 64", "81".repeat(64) + "01"],
	["nested 65", "81".repeat(65) + "01"],
].map(([name, hexText]) => ({ name, hex: hexText }));

const encoded = encodeValues.map(({ name, json }) => {
	const value = JSON.parse(json);
	try {
		const cbor = encodeCbor(value);
		return { name, json, accepted: true, cbor: hex(cbor), frame: hex(encodeFrame(cbor)) };
	} catch (error) {
		return { name, json, accepted: false, error: (error as Error).message };
	}
});

const decoded = rejectOrAccept.map(({ name, hex: input }) => {
	try {
		const value = decodeCbor(fromHex(input));
		return { name, hex: input, accepted: true, json: JSON.stringify(value) };
	} catch (error) {
		return { name, hex: input, accepted: false, error: (error as Error).message };
	}
});

writeFileSync(process.argv[2]!, `${JSON.stringify({ encode: encoded, decode: decoded }, null, 1)}\n`);
console.log(`encode ${encoded.length} (${encoded.filter((entry) => entry.accepted).length} accepted), decode ${decoded.length} (${decoded.filter((entry) => entry.accepted).length} accepted)`);
