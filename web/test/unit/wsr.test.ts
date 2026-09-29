import assert from "node:assert/strict";
import { test } from "node:test";
import {
  ChunkType, chunkTypeName, crc32, openWsr, WsrFormatError, type InputRecord, type PdrwRecord,
} from "../../src/sdk/wsr";

// ---------------------------------------------------------------- a tiny in-memory .wsr, for the reader tests only
async function deflateRaw(data: Uint8Array): Promise<Uint8Array> {
  const stream = new Blob([data.slice()]).stream().pipeThrough(new CompressionStream("deflate-raw"));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

function le32(v: number): Uint8Array {
  const b = new Uint8Array(4);
  new DataView(b.buffer).setUint32(0, v, true);
  return b;
}
function le64(v: number): Uint8Array {
  const b = new Uint8Array(8);
  new DataView(b.buffer).setBigUint64(0, BigInt(v), true);
  return b;
}
function concat(...parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((a, p) => a + p.length, 0));
  let o = 0;
  for (const p of parts) {
    out.set(p, o);
    o += p.length;
  }
  return out;
}

async function encodeChunk(type: number, data: Uint8Array, deflate: boolean): Promise<Uint8Array> {
  let stored = data, flags = 0;
  if (deflate && data.length >= 64) {
    const packed = await deflateRaw(data);
    if (packed.length < data.length) {
      stored = packed;
      flags = 1;
    }
  }
  return concat(le32(type), le32(flags), le32(data.length), le32(stored.length), le32(crc32(stored)), stored);
}

interface RawChunk { type: number; data: Uint8Array; deflate?: boolean; tickFrom?: number; tickTo?: number }

// Builds a .wsr buffer exactly as src/wormsign/format.cpp's Writer would; `trailer: false` leaves off the INDX
// and trailer, the same shape a crash or Stop-Process would leave.
async function buildWsr(chunks: RawChunk[], opts: { trailer?: boolean } = {}): Promise<Uint8Array> {
  const parts: Uint8Array[] = [new TextEncoder().encode("WSR1")];
  const index: { type: number; offset: number; tickFrom: number; tickTo: number }[] = [];
  let offset = 4;
  for (const c of chunks) {
    const encoded = await encodeChunk(c.type, c.data, c.deflate ?? true);
    index.push({ type: c.type, offset, tickFrom: c.tickFrom ?? 0, tickTo: c.tickTo ?? 0 });
    parts.push(encoded);
    offset += encoded.length;
  }
  if (opts.trailer !== false) {
    const idxBytes = concat(...index.map((e) => concat(le32(e.type), le64(e.offset), le32(e.tickFrom), le32(e.tickTo))));
    const indexAt = offset;
    const indexChunk = await encodeChunk(ChunkType.INDX, idxBytes, false);
    parts.push(indexChunk);
    offset += indexChunk.length;
    parts.push(concat(le64(indexAt), new TextEncoder().encode("WSRE")));
  }
  return concat(...parts);
}

function head(extra: Record<string, unknown> = {}): Uint8Array {
  return new TextEncoder().encode(
    JSON.stringify({ format: 1, engineHash: 1, exeBuild: "1077", melange: "0.0.0-test", startUnix: 1700000000, online: false, ...extra }),
  );
}

interface RawTick { engine: bigint; mods: bigint; c: bigint[]; rngLogic: number; rng2: number; fpucw: number; inputs: number }
function tickBytes(records: RawTick[]): Uint8Array {
  const out = new Uint8Array(records.length * 76);
  const dv = new DataView(out.buffer);
  records.forEach((r, i) => {
    const o = i * 76;
    dv.setBigUint64(o, r.engine, true);
    dv.setBigUint64(o + 8, r.mods, true);
    r.c.forEach((v, j) => dv.setBigUint64(o + 16 + j * 8, v, true));
    dv.setUint32(o + 64, r.rngLogic, true);
    dv.setUint32(o + 68, r.rng2, true);
    dv.setUint16(o + 72, r.fpucw, true);
    dv.setUint16(o + 74, r.inputs, true);
  });
  return out;
}

function inptBytes(records: InputRecord[]): Uint8Array {
  const enc = new TextEncoder();
  return concat(
    ...records.map((r) => {
      const str = enc.encode(r.str);
      const buf = new Uint8Array(24 + str.length);
      const dv = new DataView(buf.buffer);
      dv.setUint8(0, r.type);
      dv.setUint16(1, r.id, true);
      dv.setUint32(3, r.a, true);
      dv.setUint32(7, r.b, true);
      dv.setUint32(11, r.time, true);
      dv.setUint32(15, r.callT, true);
      dv.setUint32(19, r.caller, true);
      dv.setUint8(23, str.length);
      buf.set(str, 24);
      return buf;
    }),
  );
}

function pdrwBytes(records: PdrwRecord[]): Uint8Array {
  const out = new Uint8Array(records.length * 13);
  const dv = new DataView(out.buffer);
  records.forEach((r, i) => {
    const o = i * 13;
    dv.setUint8(o, r.rng);
    dv.setUint32(o + 1, r.ret, true);
    dv.setUint32(o + 5, r.stateAfter, true);
    dv.setUint32(o + 9, r.bits, true);
  });
  return out;
}

// ---------------------------------------------------------------- tests
test("crc32 matches the standard check value for \"123456789\"", () => {
  assert.equal(crc32(new TextEncoder().encode("123456789")), 0xcbf43926);
});

test("chunkTypeName names the known chunk types and falls back to hex for anything else", () => {
  assert.equal(chunkTypeName(ChunkType.TICK), "TICK");
  assert.equal(chunkTypeName(ChunkType.HEAD), "HEAD");
  assert.equal(chunkTypeName(0), "0x00000000");
});

test("openWsr reads a complete file: HEAD fields, tick numbers from the index, and completeness", async () => {
  const ticks = tickBytes([
    { engine: 0x1111111122222222n, mods: 0n, c: [1n, 2n, 3n, 4n, 5n, 6n], rngLogic: 7, rng2: 8, fpucw: 0x027f, inputs: 2 },
    { engine: 0x3333333344444444n, mods: 0n, c: [1n, 2n, 3n, 4n, 5n, 6n], rngLogic: 7, rng2: 8, fpucw: 0x027f, inputs: 0 },
  ]);
  const buf = await buildWsr([
    { type: ChunkType.HEAD, data: head(), deflate: false },
    { type: ChunkType.TICK, data: ticks, tickFrom: 10, tickTo: 11 },
  ]);
  const w = await openWsr(buf.buffer);
  assert.equal(w.complete, true);
  assert.equal(w.header.exeBuild, "1077");
  assert.equal(w.header.melange, "0.0.0-test");
  assert.equal(w.ticks.length, 2);
  assert.deepEqual(w.ticks.map((t) => t.tick), [10, 11]);
  assert.equal(w.ticks[0].engine, "1111111122222222");
  assert.equal(w.ticks[1].engine, "3333333344444444");
  assert.equal(w.ticks[0].fpucw, 0x027f);
  assert.equal(w.ticks[0].c[2], "0000000000000003");
  assert.deepEqual(w.tickRange, { from: 10, to: 11 });
});

test("openWsr marks a file without a trailer incomplete, but still reads what was written", async () => {
  const buf = await buildWsr(
    [
      { type: ChunkType.HEAD, data: head(), deflate: false },
      { type: ChunkType.TICK, data: tickBytes([{ engine: 1n, mods: 2n, c: [0n, 0n, 0n, 0n, 0n, 0n], rngLogic: 0, rng2: 0, fpucw: 0x027f, inputs: 0 }]) },
    ],
    { trailer: false },
  );
  const w = await openWsr(buf.buffer);
  assert.equal(w.complete, false);
  assert.equal(w.ticks.length, 1);
  assert.equal(w.ticks[0].tick, 0, "no index to say otherwise: ticks are assumed contiguous from 0");
});

test("openWsr stops at the first bad chunk (tail recovery) instead of throwing", async () => {
  const CHUNK_HEADER_BYTES = 20;
  const headChunk = await encodeChunk(ChunkType.HEAD, head(), false);
  const buf = await buildWsr([
    { type: ChunkType.HEAD, data: head(), deflate: false },
    { type: ChunkType.NOTE, data: new TextEncoder().encode('{"ok":true}'), deflate: false },
  ]);
  const corrupted = new Uint8Array(buf);
  corrupted[headChunk.length + CHUNK_HEADER_BYTES + 1] ^= 0xff; // a byte inside the NOTE chunk's stored payload
  const w = await openWsr(corrupted.buffer);
  assert.equal(w.complete, false, "no valid trailer once a chunk fails its CRC");
  assert.equal(w.notes.length, 0, "the corrupted NOTE chunk never made it into chunks[]");
  assert.equal(w.header.exeBuild, "1077", "the HEAD chunk before it is still readable");
});

test("openWsr refuses a format newer than it supports", async () => {
  const buf = await buildWsr([{ type: ChunkType.HEAD, data: head({ format: 2 }), deflate: false }]);
  await assert.rejects(openWsr(buf.buffer), (e) => e instanceof WsrFormatError);
});

test("openWsr refuses a file with the wrong magic", async () => {
  const buf = new TextEncoder().encode("NOPE" + "x".repeat(40));
  await assert.rejects(openWsr(buf.buffer), (e) => e instanceof WsrFormatError);
});

test("openWsr.inputs() decodes INPT's fixed layout plus the trailing string", async () => {
  const recs: InputRecord[] = [
    { type: 3, id: 100, a: 1, b: 0, time: 200, callT: 180, caller: 0x505c55, str: "" },
    { type: 6, id: 200, a: 0, b: 0, time: 220, callT: 220, caller: 0x543130, str: "hello" },
  ];
  const buf = await buildWsr([
    { type: ChunkType.HEAD, data: head(), deflate: false },
    { type: ChunkType.INPT, data: inptBytes(recs), deflate: false },
  ]);
  const w = await openWsr(buf.buffer);
  assert.deepEqual(await w.inputs(), recs);
});

test("openWsr decodes PDRW's fixed-width pre-match draw records", async () => {
  const recs: PdrwRecord[] = [{ rng: 0, ret: 0x4ee250, stateAfter: 12345, bits: 6 }, { rng: 1, ret: 0x4ee256, stateAfter: 999, bits: 3 }];
  const buf = await buildWsr([
    { type: ChunkType.HEAD, data: head(), deflate: false },
    { type: ChunkType.PDRW, data: pdrwBytes(recs), deflate: false },
  ]);
  const w = await openWsr(buf.buffer);
  assert.deepEqual(w.pdrw, recs);
});

test("openWsr decodes DVRG/NOTE generically as JSON, and falls back to a raw marker otherwise", async () => {
  const buf = await buildWsr([
    { type: ChunkType.HEAD, data: head(), deflate: false },
    { type: ChunkType.DVRG, data: new TextEncoder().encode('{"tick":37,"source":"peer"}'), deflate: false },
    { type: ChunkType.NOTE, data: new Uint8Array([1, 2, 3, 4]), deflate: false }, // not JSON
  ]);
  const w = await openWsr(buf.buffer);
  assert.deepEqual(w.divergences, [{ tick: 37, source: "peer" }]);
  assert.deepEqual(w.notes, [{ raw: true, bytes: 4 }]);
});

test("openWsr inflates a deflated chunk transparently", async () => {
  const payload = new TextEncoder().encode(JSON.stringify({ hello: "world".repeat(20) }));
  const buf = await buildWsr([
    { type: ChunkType.HEAD, data: head(), deflate: false },
    { type: ChunkType.NOTE, data: payload, deflate: true },
  ]);
  const w = await openWsr(buf.buffer);
  assert.deepEqual(w.notes, [JSON.parse(new TextDecoder().decode(payload))]);
});

test("openWsr.rawChunksOf returns raw bytes for a chunk type it does not decode itself", async () => {
  const buf = await buildWsr([
    { type: ChunkType.HEAD, data: head(), deflate: false },
    { type: ChunkType.DISP, data: new Uint8Array([9, 9, 9]), deflate: false },
  ]);
  const w = await openWsr(buf.buffer);
  const disp = await w.rawChunksOf(ChunkType.DISP);
  assert.equal(disp.length, 1);
  assert.deepEqual([...disp[0].bytes], [9, 9, 9]);
});
