// ini.get's keys grouped by section, plus the edit rules the server applies (checked here first for quick feedback).
export interface IniKey {
  section: string; key: string;
  def: string | null;       // null: not declared by any module
  live: boolean;            // applies without a restart
  declared: boolean;
  current: string | null;   // null: not in the file (the default applies)
  line?: number;
}
export interface IniDoc { path: string; encoding: string; text: string; keys: IniKey[]; }

export function docOf(v: unknown): IniDoc | undefined {
  if (!v || typeof v !== "object") return undefined;
  const o = v as Record<string, unknown>;
  const keys: IniKey[] = [];
  for (const k of Array.isArray(o.keys) ? o.keys : []) {
    if (!k || typeof k !== "object") continue;
    const x = k as Record<string, unknown>;
    if (typeof x.section !== "string" || typeof x.key !== "string") continue;
    keys.push({
      section: x.section, key: x.key,
      def: typeof x.def === "string" ? x.def : null,
      live: x.live === true, declared: x.declared !== false && typeof x.def === "string",
      current: typeof x.current === "string" ? x.current : null,
      line: typeof x.line === "number" ? x.line : undefined,
    });
  }
  return { path: typeof o.path === "string" ? o.path : "", encoding: typeof o.encoding === "string" ? o.encoding : "",
    text: typeof o.text === "string" ? o.text : "", keys };
}

export interface Section { name: string; keys: IniKey[]; }

// Sections in first-seen order, case-insensitively merged; keys sorted by name.
export function sections(keys: readonly IniKey[], filter = ""): Section[] {
  const f = filter.trim().toLowerCase();
  const map = new Map<string, Section>();
  for (const k of keys) {
    if (f && !`${k.section} ${k.key} ${k.current ?? ""}`.toLowerCase().includes(f)) continue;
    const id = k.section.toLowerCase();
    let s = map.get(id);
    if (!s) map.set(id, (s = { name: k.section, keys: [] }));
    s.keys.push(k);
  }
  for (const s of map.values()) s.keys.sort((a, b) => a.key.localeCompare(b.key));
  return [...map.values()];
}

export function effective(k: IniKey): string {
  return k.current ?? k.def ?? "";
}

export function isDefault(k: IniKey): boolean {
  return k.current === null || (k.def !== null && k.current === k.def);
}

// Mirrors the server's value rules; undefined when the value is acceptable.
export function valueProblem(v: string): string | undefined {
  if (v.length > 1000) return "longer than 1000 characters";
  if (/[\r\n\0]/.test(v)) return "line breaks are not allowed";
  if (v.includes(";")) return "';' starts a comment in Melange.ini";
  if (/^[ \t]|[ \t]$/.test(v)) return "no spaces at the start or the end";
  if (/[\x01-\x08\x0b\x0c\x0e-\x1f]/.test(v)) return "control characters are not allowed";
  return undefined;
}

// Keys the page never changes (the server refuses them too): Deep Desert can be revoked here but never granted.
export function protectedReason(section: string, key: string, value?: string): string | undefined {
  if (section.toLowerCase() !== "thumper") return undefined;
  const k = key.toLowerCase();
  if (k === "grantsalt") return "changed only in Melange.ini itself";
  if (k === "autograntdeepdesert" && value !== undefined && value !== "0") return "Deep Desert can be granted only in the game";
  return undefined;
}

export function isBoolish(k: IniKey): boolean {
  const d = k.def;
  return d === "0" || d === "1";
}
