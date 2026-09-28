// Turns a .mcap ArrayBuffer into everything the panel renders. No network or DOM assumptions beyond Blob URLs.
import { ZipReader } from "./zip";
import {
  checkManifest, markCalls, parseJsonl, programBindings,
  type CallRecord, type EventRecord, type Manifest, type Marks, type ProgramBinding, type ProgramEntry, type TextureEntry,
} from "./mcap";

export interface LoadedTexture extends TextureEntry { url?: string }
export interface LoadedProgram extends ProgramEntry { asmText?: string }

export interface LoadedCapture {
  manifest: Manifest;
  calls: CallRecord[];
  events: EventRecord[];
  marks: Marks;
  bindings: ProgramBinding[];
  begin?: Record<string, unknown>;
  end?: Record<string, unknown>;
  textures: LoadedTexture[];
  programs: LoadedProgram[];
  frameUrl?: string;
  zip: ZipReader;
  close(): void;
}

async function stateValues(zip: ZipReader, name: string): Promise<Record<string, unknown> | undefined> {
  if (!zip.find(name)) return undefined;
  const doc = await zip.json<{ values?: Record<string, unknown> }>(name);
  return doc.values;
}

export async function loadCapture(buf: ArrayBuffer): Promise<LoadedCapture> {
  const zip = ZipReader.open(buf);
  const manifest = await zip.json<unknown>("manifest.json");
  checkManifest(manifest);

  const calls = zip.find("calls.jsonl") ? parseJsonl<CallRecord>(await zip.lines("calls.jsonl")) : [];
  const events = zip.find("events.jsonl") ? parseJsonl<EventRecord>(await zip.lines("events.jsonl")) : [];
  const marks = markCalls(calls.length, events);
  const bindings = programBindings(events);

  const [begin, end] = await Promise.all([stateValues(zip, "state/begin.json"), stateValues(zip, "state/end.json")]);

  const programs: LoadedProgram[] = zip.find("programs/index.json")
    ? (await zip.json<{ programs: ProgramEntry[] }>("programs/index.json")).programs
    : [];

  const urls: string[] = [];
  const textures: LoadedTexture[] = [];
  if (zip.find("textures/index.json")) {
    const index = await zip.json<{ textures: TextureEntry[] }>("textures/index.json");
    for (const t of index.textures) {
      if (!t.file || !zip.find(t.file)) {
        textures.push({ ...t });
        continue;
      }
      const bytes = await zip.bytes(t.file);
      const url = URL.createObjectURL(new Blob([bytes.slice()], { type: "image/png" }));
      urls.push(url);
      textures.push({ ...t, url });
    }
  }

  let frameUrl: string | undefined;
  if (zip.find("frame.png")) {
    frameUrl = URL.createObjectURL(new Blob([(await zip.bytes("frame.png")).slice()], { type: "image/png" }));
    urls.push(frameUrl);
  }

  return {
    manifest, calls, events, marks, bindings, begin, end, textures, programs, frameUrl, zip,
    close() {
      for (const u of urls) URL.revokeObjectURL(u);
    },
  };
}

export async function loadProgramAsm(zip: ZipReader, program: LoadedProgram): Promise<string> {
  if (program.asmText !== undefined) return program.asmText;
  if (!program.asm) return "";
  return zip.find(program.asm) ? zip.text(program.asm) : "";
}
