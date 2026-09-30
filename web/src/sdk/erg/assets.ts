// D: the GET /erg/assets/<key>.glb|.png route. Keys only ever come from a server result (level.palette's
// "preview" field, or a future level.load use); this module never invents one, it only shapes the URL and lets
// the caller sanity-check a key before fetching it.
const KEY_RE = /^(mesh|atlas)_[a-z0-9_.-]{1,96}$/;

export function isValidPreviewKey(key: string): boolean {
  return KEY_RE.test(key) && !key.includes("..");
}

// "glb" for a detail mesh, "png" for a theme atlas; null if key is not a shape erg::preview would produce.
export function previewExt(key: string): "glb" | "png" | null {
  if (!isValidPreviewKey(key)) return null;
  return key.startsWith("mesh_") ? "glb" : "png";
}

// The route's path for a key, e.g. "/erg/assets/mesh_building15.glb". "" for an empty key (no preview - the
// caller draws a labelled box) or one that does not parse.
export function assetUrl(key: string): string {
  if (!key) return "";
  const ext = previewExt(key);
  return ext ? `/erg/assets/${key}.${ext}` : "";
}
