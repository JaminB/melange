// A minimal ZIP reader for .mcap files: central-directory parsing plus DEFLATE via the platform's
// DecompressionStream. No zip library (docs/capture-format.md, M3 §4 component E).
const EOCD_SIGNATURE = 0x06054b50;
const CENTRAL_SIGNATURE = 0x02014b50;
const LOCAL_SIGNATURE = 0x04034b50;
const EOCD_SEARCH_WINDOW = 22 + 65535;

export class ZipFormatError extends Error {}

export interface ZipEntry {
  name: string;
  method: number; // 0 = stored, 8 = deflate
  compressedSize: number;
  size: number;
  crc32: number;
  localHeaderOffset: number;
}

function findEndOfCentralDirectory(view: DataView): number {
  const start = Math.max(0, view.byteLength - EOCD_SEARCH_WINDOW);
  for (let i = view.byteLength - 22; i >= start; i--) {
    if (view.getUint32(i, true) === EOCD_SIGNATURE) return i;
  }
  throw new ZipFormatError("not a zip file (no end-of-central-directory record found)");
}

export class ZipReader {
  private constructor(private readonly buf: ArrayBuffer, readonly entries: readonly ZipEntry[]) {}

  static open(buf: ArrayBuffer): ZipReader {
    const view = new DataView(buf);
    const eocd = findEndOfCentralDirectory(view);
    const count = view.getUint16(eocd + 10, true);
    let offset = view.getUint32(eocd + 16, true);
    const bytes = new Uint8Array(buf);
    const decoder = new TextDecoder();
    const entries: ZipEntry[] = [];
    for (let i = 0; i < count; i++) {
      if (offset + 46 > view.byteLength || view.getUint32(offset, true) !== CENTRAL_SIGNATURE)
        throw new ZipFormatError(`truncated central directory (entry ${i + 1} of ${count})`);
      const method = view.getUint16(offset + 10, true);
      const crc32Value = view.getUint32(offset + 16, true);
      const compressedSize = view.getUint32(offset + 20, true);
      const size = view.getUint32(offset + 24, true);
      const nameLen = view.getUint16(offset + 28, true);
      const extraLen = view.getUint16(offset + 30, true);
      const commentLen = view.getUint16(offset + 32, true);
      const localHeaderOffset = view.getUint32(offset + 42, true);
      if (offset + 46 + nameLen > view.byteLength) throw new ZipFormatError("truncated central directory entry name");
      const name = decoder.decode(bytes.subarray(offset + 46, offset + 46 + nameLen));
      entries.push({ name, method, compressedSize, size, crc32: crc32Value, localHeaderOffset });
      offset += 46 + nameLen + extraLen + commentLen;
    }
    return new ZipReader(buf, entries);
  }

  find(name: string): ZipEntry | undefined {
    return this.entries.find((e) => e.name === name);
  }

  async bytes(name: string): Promise<Uint8Array> {
    const entry = this.find(name);
    if (!entry) throw new ZipFormatError(`missing ${name} in the capture`);
    return this.entryBytes(entry);
  }

  async entryBytes(entry: ZipEntry): Promise<Uint8Array> {
    const view = new DataView(this.buf);
    const off = entry.localHeaderOffset;
    if (off + 30 > this.buf.byteLength || view.getUint32(off, true) !== LOCAL_SIGNATURE)
      throw new ZipFormatError(`truncated local file header for ${entry.name}`);
    const nameLen = view.getUint16(off + 26, true);
    const extraLen = view.getUint16(off + 28, true);
    const dataStart = off + 30 + nameLen + extraLen;
    const dataEnd = dataStart + entry.compressedSize;
    if (dataEnd > this.buf.byteLength) throw new ZipFormatError(`truncated data for ${entry.name}`);
    const raw = new Uint8Array(this.buf, dataStart, entry.compressedSize);
    let out: Uint8Array;
    if (entry.method === 0) out = raw.slice();
    else if (entry.method === 8) out = await inflateRaw(raw);
    else throw new ZipFormatError(`${entry.name}: unsupported zip method ${entry.method}`);
    if (out.length !== entry.size)
      throw new ZipFormatError(`${entry.name}: decompressed size mismatch (expected ${entry.size}, got ${out.length})`);
    if (crc32(out) !== entry.crc32) throw new ZipFormatError(`${entry.name}: checksum mismatch (the file may be corrupt)`);
    return out;
  }

  async text(name: string): Promise<string> {
    return new TextDecoder().decode(await this.bytes(name));
  }

  async json<T>(name: string): Promise<T> {
    return JSON.parse(await this.text(name)) as T;
  }

  async lines(name: string): Promise<string[]> {
    return (await this.text(name)).split("\n").filter((l) => l.length > 0);
  }
}

async function inflateRaw(data: Uint8Array): Promise<Uint8Array> {
  let stream: ReadableStream<Uint8Array>;
  try {
    stream = new Blob([data.slice()]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
  } catch (e) {
    throw new ZipFormatError(`deflate stream rejected: ${e instanceof Error ? e.message : String(e)}`);
  }
  try {
    return new Uint8Array(await new Response(stream).arrayBuffer());
  } catch (e) {
    throw new ZipFormatError(`deflate stream rejected: ${e instanceof Error ? e.message : String(e)}`);
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
