// A generic recursive diff between two JSON-like values, for comparing two sides' detail records or any other
// JSON the desync bundle carries. Field-level, one row per leaf that differs, named as a dotted/bracketed path
// ("worms[3].energy") the way the bundle's own diff.txt does.
export interface DiffRow { path: string; before: unknown; after: unknown }

function isPlainObject(x: unknown): x is Record<string, unknown> {
  return !!x && typeof x === "object" && !Array.isArray(x);
}

export function deepDiff(a: unknown, b: unknown, path = ""): DiffRow[] {
  if (a === b) return [];
  if (isPlainObject(a) && isPlainObject(b)) {
    const keys = [...new Set([...Object.keys(a), ...Object.keys(b)])].sort();
    return keys.flatMap((k) => deepDiff(a[k], b[k], path ? `${path}.${k}` : k));
  }
  if (Array.isArray(a) && Array.isArray(b)) {
    const rows: DiffRow[] = [];
    for (let i = 0; i < Math.max(a.length, b.length); i++) rows.push(...deepDiff(a[i], b[i], `${path}[${i}]`));
    return rows;
  }
  return [{ path: path || "(root)", before: a, after: b }];
}

// A bundle name from a divergence's `bundle` field or a wormsign.divergence event, for building the /replays/
// download link (bundles live in the same folder as recordings).
export function bundleUrl(name: string): string {
  return `/replays/${encodeURIComponent(name)}`;
}
