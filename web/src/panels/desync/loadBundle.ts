// Turns a desync bundle .zip into what the panel renders. Reuses the capture viewer's dependency-free ZipReader
// (web/src/panels/capture/zip.ts) rather than a second zip implementation. Every field here is still read
// defensively and nothing is assumed about report.json or the detail-*.json shape beyond "valid JSON" --
// diff.txt (a plain field-level diff, one line per field) is shown verbatim regardless, so the viewer keeps
// working even if those JSON shapes change later.
import { ZipReader } from "../capture/zip";

export interface LoadedBundle {
  reportJson?: unknown;
  diffText?: string;
  detailLocal?: unknown;
  detailPeer?: unknown;
  files: string[];
  zip: ZipReader;
}

async function tryJson(zip: ZipReader, name: string): Promise<unknown> {
  if (!zip.find(name)) return undefined;
  try {
    return await zip.json<unknown>(name);
  } catch {
    return undefined;
  }
}

export async function loadBundle(buf: ArrayBuffer): Promise<LoadedBundle> {
  const zip = ZipReader.open(buf);
  const [reportJson, detailLocal, detailPeer] = await Promise.all([
    tryJson(zip, "report.json"), tryJson(zip, "detail-local.json"), tryJson(zip, "detail-peer.json"),
  ]);
  const diffText = zip.find("diff.txt") ? await zip.text("diff.txt") : undefined;
  return { reportJson, diffText, detailLocal, detailPeer, files: zip.entries.map((e) => e.name), zip };
}
