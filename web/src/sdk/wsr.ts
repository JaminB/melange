// The .wsr recording container reader, mirroring src/wormsign/format.cpp exactly at the container level: magic,
// chunk headers, CRC32, raw deflate, the INDX/trailer and the same tail-recovery rule a truncated file gets from
// the C++ reader (walk chunks until the first bad one, mark the file incomplete).
//
// Chunk *payloads* are a mix of what has already shipped and what a later writer still owns:
//   - HEAD and NOTE are JSON; decoded as such.
//   - TICK is `TickHash` (melange/wormsign.h) without the leading `tick` field -- a frozen, fixed-size ABI --
//     decoded field by field below.
//   - INPT's layout is given explicitly by the design (u8/u16/u32 fields, a trailing string); decoded on that
//     basis, but no writer has shipped for it yet, so treat this as best-effort until a real recording confirms it.
//   - SEED, PDRW, SETP, RMTI, DISP, ENGV and DVRG have no shipped writer at all yet. They are decoded generically:
//     JSON if the payload parses as JSON, otherwise left as raw bytes. Nothing here assumes a specific layout for
//     them, so this reader keeps working once a writer lands, whichever encoding it picks.
export class WsrFormatError extends Error {}

const MAGIC = "WSR1";
const TRAILER_TAG = "WSRE";
const CHUNK_HEADER_BYTES = 20;
const INDEX_ENTRY_BYTES = 20;
const TRAILER_BYTES = 12;
const FLAG_DEFLATE = 1;
const FORMAT_SUPPORTED = 1;
const TICK_RECORD_BYTES = 76; // engine(8) mods(8) c[6](48) rngLogic(4) rng2(4) fpucw(2) inputs(2)
const INPT_FIXED_BYTES = 24; // type(1) id(2) a(4) b(4) time(4) callT(4) caller(4) strLen(1)

function tag(s: string): number {
  return s.charCodeAt(0) | (s.charCodeAt(1) << 8) | (s.charCodeAt(2) << 16) | (s.charCodeAt(3) << 24);
}

export const ChunkType = {
  HEAD: tag("HEAD"), SEED: tag("SEED"), PDRW: tag("PDRW"), SETP: tag("SETP"), INPT: tag("INPT"),
  RMTI: tag("RMTI"), DISP: tag("DISP"), TICK: tag("TICK"), DETL: tag("DETL"), DVRG: tag("DVRG"),
  ENGV: tag("ENGV"), NOTE: tag("NOTE"), INDX: tag("INDX"),
} as const;

const TYPE_NAMES = new Map<number, string>(Object.entries(ChunkType).map(([k, v]) => [v, k]));
export function chunkTypeName(type: number): string {
  return TYPE_NAMES.get(type) ?? `0x${type.toString(16).padStart(8, "0")}`;
}

export interface ChunkRef { type: number; flags: number; rawLen: number; storedLen: number; offset: number }
export interface IndexEntry { type: number; offset: number; tickFrom: number; tickTo: number }

export interface WsrHeader {
  format: number;
  engineHash?: number;
  exeBuild?: string;
  exeSha256?: string;
  melange?: string;
  startUnix?: number;
  online?: boolean;
  localNet?: boolean;
  contentHash?: string;
  tickMs?: number;
  [key: string]: unknown;
}

// engine/mods/c are lowercase 16-hex-digit strings (matching the server's "%016llx"), not bigint: every consumer
// here only displays or compares them, and hex strings compare byte-for-byte just as well as the raw 64-bit value.
export interface TickRecord {
  tick: number;
  engine: string;
  mods: string;
  c: string[];
  rngLogic: number;
  rng2: number;
  fpucw: number;
  inputs: number;
}

export interface InputRecord {
  type: number; id: number; a: number; b: number; time: number; callT: number; caller: number; str: string;
}

export interface PdrwRecord { rng: number; ret: number; stateAfter: number; bits: number }
const PDRW_RECORD_BYTES = 13; // rng(1) ret(4) stateAfter(4) bits(4)

export interface WsrFile {
  header: WsrHeader;
  format: number;
  complete: boolean;
  chunks: readonly ChunkRef[];
  index: readonly IndexEntry[];
  ticks: readonly TickRecord[];
  tickRange?: { from: number; to: number };
  divergences: readonly unknown[];
  notes: readonly unknown[];
  setp: unknown;
  seeds: readonly unknown[];
  engv: readonly unknown[];
  pdrw: readonly PdrwRecord[];
  // Decodes every chunk of `type` on demand (INPT, RMTI, DISP, DETL and any other type are not eagerly decoded).
  inputs(): Promise<InputRecord[]>;
  rawChunksOf(type: number): Promise<{ ref: ChunkRef; bytes: Uint8Array }[]>;
}

function hex64(dv: DataView, offset: number): string {
  const lo = dv.getUint32(offset, true), hi = dv.getUint32(offset + 4, true);
  return hi.toString(16).padStart(8, "0") + lo.toString(16).padStart(8, "0");
}

function decodeTicks(bytes: Uint8Array, tickFrom: number): TickRecord[] {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const count = Math.floor(bytes.length / TICK_RECORD_BYTES);
  const out: TickRecord[] = [];
  for (let i = 0; i < count; i++) {
    const o = i * TICK_RECORD_BYTES;
    const c: string[] = [];
    for (let j = 0; j < 6; j++) c.push(hex64(dv, o + 16 + j * 8));
    out.push({
      tick: tickFrom + i,
      engine: hex64(dv, o), mods: hex64(dv, o + 8), c,
      rngLogic: dv.getUint32(o + 64, true), rng2: dv.getUint32(o + 68, true),
      fpucw: dv.getUint16(o + 72, true), inputs: dv.getUint16(o + 74, true),
    });
  }
  return out;
}

function decodeInputs(bytes: Uint8Array): InputRecord[] {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const dec = new TextDecoder();
  const out: InputRecord[] = [];
  let o = 0;
  while (o + INPT_FIXED_BYTES <= bytes.length) {
    const strLen = dv.getUint8(o + 23);
    if (o + INPT_FIXED_BYTES + strLen > bytes.length) break; // truncated trailing record: stop, keep what parsed
    out.push({
      type: dv.getUint8(o), id: dv.getUint16(o + 1, true), a: dv.getUint32(o + 3, true), b: dv.getUint32(o + 7, true),
      time: dv.getUint32(o + 11, true), callT: dv.getUint32(o + 15, true), caller: dv.getUint32(o + 19, true),
      str: strLen ? dec.decode(bytes.subarray(o + INPT_FIXED_BYTES, o + INPT_FIXED_BYTES + strLen)) : "",
    });
    o += INPT_FIXED_BYTES + strLen;
  }
  return out;
}

function decodePdrw(bytes: Uint8Array): PdrwRecord[] {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const count = Math.floor(bytes.length / PDRW_RECORD_BYTES);
  const out: PdrwRecord[] = [];
  for (let i = 0; i < count; i++) {
    const o = i * PDRW_RECORD_BYTES;
    out.push({ rng: dv.getUint8(o), ret: dv.getUint32(o + 1, true), stateAfter: dv.getUint32(o + 5, true), bits: dv.getUint32(o + 9, true) });
  }
  return out;
}

function tryJson(bytes: Uint8Array): unknown {
  try {
    return JSON.parse(new TextDecoder().decode(bytes));
  } catch {
    return undefined;
  }
}

async function inflateRaw(data: Uint8Array): Promise<Uint8Array> {
  try {
    const stream = new Blob([data.slice()]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  } catch (e) {
    throw new WsrFormatError(`deflate stream rejected: ${e instanceof Error ? e.message : String(e)}`);
  }
}

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c >>> 0;
  }
  return table;
})();
export function crc32(data: Uint8Array): number {
  let c = 0xffffffff;
  for (let i = 0; i < data.length; i++) c = CRC_TABLE[(c ^ data[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

async function payloadOf(bytes: Uint8Array, ref: ChunkRef): Promise<Uint8Array> {
  const stored = bytes.subarray(ref.offset + CHUNK_HEADER_BYTES, ref.offset + CHUNK_HEADER_BYTES + ref.storedLen);
  if (!(ref.flags & FLAG_DEFLATE)) return stored;
  const out = await inflateRaw(stored);
  if (out.length !== ref.rawLen) throw new WsrFormatError(`chunk at ${ref.offset}: inflated to ${out.length} bytes, expected ${ref.rawLen}`);
  return out;
}

function parseContainer(bytes: Uint8Array): { chunks: ChunkRef[]; index: IndexEntry[]; complete: boolean } {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const magic = new TextDecoder().decode(bytes.subarray(0, 4));
  if (bytes.length < 4 || magic !== MAGIC) throw new WsrFormatError("not a .wsr file (bad magic)");

  const chunks: ChunkRef[] = [];
  let pos = 4, indexAt = -1, sawIndex = false;
  while (pos + CHUNK_HEADER_BYTES <= bytes.length) {
    const type = dv.getUint32(pos, true), flags = dv.getUint32(pos + 4, true);
    const rawLen = dv.getUint32(pos + 8, true), storedLen = dv.getUint32(pos + 12, true), crc = dv.getUint32(pos + 16, true);
    if ((flags & ~FLAG_DEFLATE) !== 0 || storedLen > bytes.length - pos - CHUNK_HEADER_BYTES) break;
    if (!(flags & FLAG_DEFLATE) && storedLen !== rawLen) break;
    const stored = bytes.subarray(pos + CHUNK_HEADER_BYTES, pos + CHUNK_HEADER_BYTES + storedLen);
    if (crc32(stored) !== crc) break;
    const ref: ChunkRef = { type, flags, rawLen, storedLen, offset: pos };
    chunks.push(ref);
    pos += CHUNK_HEADER_BYTES + storedLen;
    if (type === ChunkType.INDX) {
      sawIndex = true;
      indexAt = ref.offset;
      break;
    }
  }
  if (chunks.length === 0 || chunks[0].type !== ChunkType.HEAD) throw new WsrFormatError("no HEAD chunk");

  const index: IndexEntry[] = [];
  let complete = false;
  if (sawIndex && pos + TRAILER_BYTES === bytes.length) {
    const trailerIndexOffset = Number(dv.getBigUint64(pos, true));
    const trailerTag = new TextDecoder().decode(bytes.subarray(pos + 8, pos + 12));
    const indexChunk = chunks[chunks.length - 1];
    if (trailerIndexOffset === indexAt && trailerTag === TRAILER_TAG && indexChunk.flags === 0 &&
        indexChunk.rawLen % INDEX_ENTRY_BYTES === 0 && indexChunk.rawLen / INDEX_ENTRY_BYTES === chunks.length - 1) {
      let q = indexChunk.offset + CHUNK_HEADER_BYTES;
      let ok = true;
      for (let i = 0; i < chunks.length - 1; i++, q += INDEX_ENTRY_BYTES) {
        const e: IndexEntry = { type: dv.getUint32(q, true), offset: Number(dv.getBigUint64(q + 4, true)),
          tickFrom: dv.getUint32(q + 12, true), tickTo: dv.getUint32(q + 16, true) };
        if (e.type !== chunks[i].type || e.offset !== chunks[i].offset) ok = false;
        index.push(e);
      }
      complete = ok;
      if (!ok) index.length = 0;
    }
  }
  return { chunks, index, complete };
}

export async function openWsr(buf: ArrayBufferLike): Promise<WsrFile> {
  const bytes = new Uint8Array(buf);
  const { chunks, index, complete } = parseContainer(bytes);

  const headBytes = await payloadOf(bytes, chunks[0]);
  const headJson = tryJson(headBytes);
  if (!headJson || typeof headJson !== "object") throw new WsrFormatError("HEAD is not a JSON object");
  const header = headJson as WsrHeader;
  if (typeof header.format !== "number" || header.format < 1) throw new WsrFormatError("HEAD has no format");
  if (header.format > FORMAT_SUPPORTED)
    throw new WsrFormatError(`format ${header.format} is newer than this reader supports (${FORMAT_SUPPORTED})`);

  const indexByOffset = new Map(index.map((e) => [e.offset, e]));
  const ticks: TickRecord[] = [];
  const divergences: unknown[] = [];
  const notes: unknown[] = [];
  const engv: unknown[] = [];
  const seeds: unknown[] = [];
  const pdrw: PdrwRecord[] = [];
  let setp: unknown;
  let nextTick = 0;
  let tickRange: { from: number; to: number } | undefined;

  for (const ref of chunks) {
    if (ref.type === ChunkType.INDX || ref.type === ChunkType.HEAD) continue;
    if (ref.type === ChunkType.TICK) {
      const entry = indexByOffset.get(ref.offset);
      const from = entry ? entry.tickFrom : nextTick;
      const payload = await payloadOf(bytes, ref);
      const records = decodeTicks(payload, from);
      ticks.push(...records);
      nextTick = from + records.length;
      if (records.length) {
        tickRange = tickRange ? { from: Math.min(tickRange.from, records[0].tick), to: Math.max(tickRange.to, records[records.length - 1].tick) }
                               : { from: records[0].tick, to: records[records.length - 1].tick };
      }
      continue;
    }
    if (ref.type === ChunkType.PDRW) {
      pdrw.push(...decodePdrw(await payloadOf(bytes, ref)));
      continue;
    }
    if (ref.type === ChunkType.DVRG || ref.type === ChunkType.NOTE || ref.type === ChunkType.ENGV ||
        ref.type === ChunkType.SETP || ref.type === ChunkType.SEED) {
      const payload = await payloadOf(bytes, ref);
      const value = tryJson(payload) ?? { raw: true, bytes: payload.length };
      if (ref.type === ChunkType.DVRG) divergences.push(value);
      else if (ref.type === ChunkType.NOTE) notes.push(value);
      else if (ref.type === ChunkType.ENGV) engv.push(value);
      else if (ref.type === ChunkType.SETP) setp = value;
      else seeds.push(value);
    }
  }

  return {
    header, format: header.format, complete, chunks, index, ticks, tickRange, divergences, notes, setp, seeds, engv, pdrw,
    async inputs() {
      const out: InputRecord[] = [];
      for (const ref of chunks.filter((c) => c.type === ChunkType.INPT)) out.push(...decodeInputs(await payloadOf(bytes, ref)));
      return out;
    },
    async rawChunksOf(type: number) {
      const out: { ref: ChunkRef; bytes: Uint8Array }[] = [];
      for (const ref of chunks.filter((c) => c.type === type)) out.push({ ref, bytes: await payloadOf(bytes, ref) });
      return out;
    },
  };
}
