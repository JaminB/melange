import assert from "node:assert/strict";
import { test } from "node:test";
import {
  activeProgramAt, checkManifest, isDraw, markCalls, parseJsonl, programBindings, stateDiff,
  UnsupportedCaptureError, type EventRecord,
} from "../../src/panels/capture/mcap";
import { crc32, ZipFormatError, ZipReader } from "../../src/panels/capture/zip";

// ---------------------------------------------------------------- a tiny in-memory zip, for the reader tests only
async function deflateRaw(data: Uint8Array): Promise<Uint8Array> {
  const stream = new Blob([data.slice()]).stream().pipeThrough(new CompressionStream("deflate-raw"));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

interface ZipFile { name: string; data: Uint8Array; method: 0 | 8 }

function put(view: DataView, off: number, values: [number, number, number][]): void {
  for (const [byteOffset, size, value] of values) {
    if (size === 2) view.setUint16(off + byteOffset, value, true);
    else view.setUint32(off + byteOffset, value, true);
  }
}

async function buildZip(files: { name: string; data: Uint8Array; deflate?: boolean }[]): Promise<ArrayBuffer> {
  const resolved: ZipFile[] = [];
  for (const f of files) resolved.push({ name: f.name, data: f.data, method: f.deflate ? 8 : 0 });
  const enc = new TextEncoder();
  const chunks: Uint8Array[] = [];
  const centralChunks: Uint8Array[] = [];
  let offset = 0;
  for (const f of resolved) {
    const raw = f.method === 8 ? await deflateRaw(f.data) : f.data;
    const crc = crc32(f.data);
    const nameBytes = enc.encode(f.name);
    const local = new Uint8Array(30 + nameBytes.length);
    const lv = new DataView(local.buffer);
    lv.setUint32(0, 0x04034b50, true);
    put(lv, 0, [[4, 2, 20], [6, 2, 0], [8, 2, f.method], [10, 2, 0], [12, 2, 0]]);
    put(lv, 0, [[14, 4, crc], [18, 4, raw.length], [22, 4, f.data.length], [26, 2, nameBytes.length], [28, 2, 0]]);
    local.set(nameBytes, 30);
    const localOffset = offset;
    chunks.push(local, raw);
    offset += local.length + raw.length;

    const central = new Uint8Array(46 + nameBytes.length);
    const cv = new DataView(central.buffer);
    cv.setUint32(0, 0x02014b50, true);
    put(cv, 0, [[4, 2, 20], [6, 2, 20], [8, 2, 0], [10, 2, f.method], [12, 2, 0], [14, 2, 0]]);
    put(cv, 0, [[16, 4, crc], [20, 4, raw.length], [24, 4, f.data.length], [28, 2, nameBytes.length]]);
    put(cv, 0, [[30, 2, 0], [32, 2, 0], [34, 2, 0], [36, 2, 0], [38, 4, 0], [42, 4, localOffset]]);
    central.set(nameBytes, 46);
    centralChunks.push(central);
  }
  const centralStart = offset;
  for (const c of centralChunks) { chunks.push(c); offset += c.length; }
  const centralSize = offset - centralStart;
  const eocd = new Uint8Array(22);
  const ev = new DataView(eocd.buffer);
  ev.setUint32(0, 0x06054b50, true);
  put(ev, 0, [[8, 2, resolved.length], [10, 2, resolved.length]]);
  ev.setUint32(12, centralSize, true);
  ev.setUint32(16, centralStart, true);
  chunks.push(eocd);
  const total = chunks.reduce((a, c) => a + c.length, 0);
  const out = new Uint8Array(total);
  let p = 0;
  for (const c of chunks) { out.set(c, p); p += c.length; }
  return out.buffer;
}

test("crc32 matches the standard check value for \"123456789\"", () => {
  assert.equal(crc32(new TextEncoder().encode("123456789")), 0xcbf43926);
});

test("ZipReader reads a stored entry and a deflated entry", async () => {
  const buf = await buildZip([
    { name: "manifest.json", data: new TextEncoder().encode('{"format":"melange-capture","version":1}') },
    { name: "calls.jsonl", data: new TextEncoder().encode('{"i":0,"fn":"glClear"}\n{"i":1,"fn":"glDrawArrays"}\n'), deflate: true },
  ]);
  const zip = ZipReader.open(buf);
  assert.equal(zip.entries.length, 2);
  const manifest = await zip.json<{ format: string; version: number }>("manifest.json");
  assert.equal(manifest.format, "melange-capture");
  const lines = await zip.lines("calls.jsonl");
  assert.deepEqual(lines.length, 2);
});

test("ZipReader rejects a truncated zip instead of throwing something opaque", async () => {
  const buf = await buildZip([{ name: "a.txt", data: new TextEncoder().encode("hello") }]);
  const truncated = buf.slice(0, buf.byteLength - 30);
  assert.throws(() => ZipReader.open(truncated), ZipFormatError);
});

test("ZipReader rejects a corrupted entry by checksum", async () => {
  const buf = await buildZip([{ name: "a.txt", data: new TextEncoder().encode("hello world") }]);
  const bytes = new Uint8Array(buf);
  const zip = ZipReader.open(buf);
  const entry = zip.find("a.txt")!;
  const dataStart = entry.localHeaderOffset + 30 + new TextEncoder().encode(entry.name).length;
  bytes[dataStart] ^= 0xff; // flip a byte inside the (stored) file data, not the header or name
  await assert.rejects(zip.bytes("a.txt"), (e) => e instanceof ZipFormatError);
});

test("checkManifest accepts version 1 and refuses a newer or foreign format", () => {
  checkManifest({ format: "melange-capture", version: 1, counts: {} });
  assert.throws(() => checkManifest({ format: "melange-capture", version: 2, counts: {} }), UnsupportedCaptureError);
  assert.throws(() => checkManifest({ format: "something-else", version: 1, counts: {} }), UnsupportedCaptureError);
  assert.throws(() => checkManifest(null), UnsupportedCaptureError);
});

test("parseJsonl skips blank lines and parses the rest", () => {
  assert.deepEqual(parseJsonl(['{"a":1}', "", '  ', '{"a":2}']), [{ a: 1 }, { a: 2 }]);
});

test("isDraw matches the capture format's draw-call prefixes only", () => {
  assert.ok(isDraw("glDrawArrays"));
  assert.ok(isDraw("glDrawArraysInstancedBaseInstance"));
  assert.ok(isDraw("glDrawRangeElements"));
  assert.ok(!isDraw("glBindTexture"));
  assert.ok(!isDraw("glDraw")); // not a real call; must not match by accident
});

test("markCalls carries frame/pass/stage forward from events.jsonl", () => {
  const events: EventRecord[] = [
    { at: 0, type: "frame-begin", frame: 100 },
    { at: 0, type: "pass", pass: 1 },
    { at: 2, type: "stage", stage: "World" },
    { at: 3, type: "pass", pass: 3 },
  ];
  const marks = markCalls(4, events);
  assert.deepEqual([...marks.frame], [100, 100, 100, 100]);
  assert.deepEqual([...marks.pass], [1, 1, 1, 3]);
  assert.deepEqual(marks.stage, [undefined, undefined, "World", "World"]);
});

test("programBindings and activeProgramAt track the currently bound ARB program", () => {
  const events: EventRecord[] = [
    { at: 1, type: "cg-bind", arb: 7 },
    { at: 5, type: "cg-bind", arb: 0 }, // unbind: not a real binding
    { at: 6, type: "cg-bind", arb: 9 },
  ];
  const bindings = programBindings(events);
  assert.deepEqual(bindings.map((b) => b.arb), [7, 9]);
  assert.equal(activeProgramAt(bindings, 0), undefined);
  assert.equal(activeProgramAt(bindings, 1), 7);
  assert.equal(activeProgramAt(bindings, 5), 7);
  assert.equal(activeProgramAt(bindings, 6), 9);
});

test("stateDiff reports only the keys that changed", () => {
  const rows = stateDiff({ a: 1, b: 2, c: [1, 2] }, { a: 1, b: 3, c: [1, 2] });
  assert.deepEqual(rows, [{ key: "b", before: 2, after: 3 }]);
});
