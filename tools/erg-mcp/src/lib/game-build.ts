// The Melange build a game folder would run: the file version of its melange.asi (its VS_FIXEDFILEINFO, found by its
// signature), without starting anything. connect and open_project compare it with what this erg-mcp and the open
// project need, so a test game left on an older build is named before a Test or an installed level fails in it.
import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";

/** The x.y.z of a PE file's fixed version info, or null when it has none. */
export function peVersion(bytes: Uint8Array): string | null {
  // VS_FIXEDFILEINFO: dwSignature 0xFEEF04BD, dwStrucVersion, dwFileVersionMS, dwFileVersionLS (little endian)
  for (let i = bytes.length - 16; i >= 0; i--) {
    if (bytes[i] !== 0xbd || bytes[i + 1] !== 0x04 || bytes[i + 2] !== 0xef || bytes[i + 3] !== 0xfe) continue;
    const u16 = (o: number) => bytes[i + o] | (bytes[i + o + 1] << 8);
    return `${u16(10)}.${u16(8)}.${u16(14)}`;
  }
  return null;
}

/** The game folder's melange.asi and its version, or null (no folder, no file, no version info). */
export function gameBuild(dir: string | undefined): { dir: string; file: string; version: string } | null {
  if (!dir) return null;
  const file = join(dir, "melange.asi");
  try {
    if (!existsSync(file)) return null;
    const version = peVersion(new Uint8Array(readFileSync(file)));
    return version ? { dir, file, version } : null;
  } catch { return null; }
}
