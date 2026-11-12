// lint_mesh_banks' rules: which engine section each mod mesh bank ends up in, as Melange's loader assigns them
// (docs/meshes.md "The section budget", src/assets/meshbank.cpp). Every enabled content mod's banks load once per launch
// in load order (mod order, then the order of its spice.json `meshes`); a bank keeps the section it was built with when
// that is one of the 44 mod sections (476..519) and still free, else it is rewritten into the first free one from 476 up;
// past 44 banks the rest do not load. So a map's bank built on a section another plugin's bank was built on moves one of
// the two, and which one depends on the plugins that are on: Dustcrown's bank and Kindjal's eleventh were both built on
// 486. Pure: the banks' facts in, the assignment out.
export const SECTION_MIN = 476, SECTION_MAX = 519, SECTIONS = SECTION_MAX - SECTION_MIN + 1;

export interface BankIn {
  mod: string;
  file: string;                 // as the mod's spice.json names it
  section: number | null;       // the SectionId it was built with; null when it could not be read
  meshes: string[];             // its mesh names (<modId>.<Name>)
  error?: string;               // why it could not be read (it then loads nothing and burns no section)
}

export interface BankOut extends BankIn {
  resolved: number | null;      // the section it loads in, or null when it does not load
  relocated: boolean;           // resolved differs from the section it was built with
  why?: string;                 // why it moved or did not load
}

export interface Assignment {
  banks: BankOut[];
  used: number; free: number;
  notLoaded: number;
  relocated: number;
  duplicateNames: { name: string; banks: string[] }[];
}

/** The loader's assignment for the banks in load order. */
export function assignSections(banks: BankIn[]): Assignment {
  const taken = new Map<number, string>(), out: BankOut[] = [];
  const seen = new Map<string, string[]>();
  for (const b of banks) {
    const label = `${b.mod}/${b.file}`;
    if (b.error || b.section === null) { out.push({ ...b, resolved: null, relocated: false, why: b.error ?? "the bank's section could not be read" }); continue; }
    const bad = b.meshes.find((n) => !n.startsWith(`${b.mod}.`));
    if (bad) { out.push({ ...b, resolved: null, relocated: false, why: `the mesh ${bad} is outside the mod's own names (${b.mod}.*): the bank is refused` }); continue; }
    const inRange = b.section >= SECTION_MIN && b.section <= SECTION_MAX;
    let sec: number | null = inRange && !taken.has(b.section) ? b.section : null, why: string | undefined;
    if (sec === null) {
      for (let s = SECTION_MIN; s <= SECTION_MAX; s++) if (!taken.has(s)) { sec = s; break; }
      why = sec === null ? `all ${SECTIONS} mod sections (${SECTION_MIN}..${SECTION_MAX}) are in use by the banks before it in load order`
        : inRange ? `section ${b.section} is taken by ${taken.get(b.section)}, which loads before it` : `section ${b.section} is outside ${SECTION_MIN}..${SECTION_MAX}`;
    }
    if (sec !== null) taken.set(sec, label);
    for (const n of b.meshes) seen.set(n, [...(seen.get(n) ?? []), label]);
    out.push({ ...b, resolved: sec, relocated: sec !== null && sec !== b.section, ...(why ? { why } : {}) });
  }
  return { banks: out, used: taken.size, free: SECTIONS - taken.size, notLoaded: out.filter((b) => b.resolved === null).length, relocated: out.filter((b) => b.relocated).length,
    duplicateNames: [...seen].filter(([, l]) => l.length > 1).map(([name, l]) => ({ name, banks: l })) };
}

export interface ModIn { id: string; dependencies: string[]; banks: BankIn[]; }

/** The mods in load order: a mod after the mods it depends on, otherwise by id (Thumper's order for mods the player has
 * not reordered). */
export function loadOrder(mods: ModIn[]): ModIn[] {
  const byId = new Map(mods.map((m) => [m.id, m])), done = new Set<string>(), out: ModIn[] = [];
  const visit = (m: ModIn, stack: Set<string>) => {
    if (done.has(m.id) || stack.has(m.id)) return;
    stack.add(m.id);
    for (const d of [...m.dependencies].sort()) { const dm = byId.get(d); if (dm) visit(dm, stack); }
    stack.delete(m.id);
    done.add(m.id);
    out.push(m);
  };
  for (const m of [...mods].sort((a, b) => (a.id < b.id ? -1 : a.id > b.id ? 1 : 0))) visit(m, new Set());
  return out;
}

/** The sections no bank of `banks` was built with, lowest first: where a new bank can be built without moving any. */
export function freeBuiltSections(banks: BankIn[]): number[] {
  const built = new Set(banks.map((b) => b.section).filter((s): s is number => s !== null));
  const out: number[] = [];
  for (let s = SECTION_MIN; s <= SECTION_MAX; s++) if (!built.has(s)) out.push(s);
  return out;
}

/** How one mod's banks fare across plugin sets: per bank, its section in each set, and whether it changes. */
export function compareSets(sets: { name: string; assignment: Assignment }[], mod: string): { file: string; meshes: string[]; built: number | null; sections: Record<string, number | null>; shifts: boolean }[] {
  const files = new Map<string, { meshes: string[]; built: number | null; sections: Record<string, number | null> }>();
  for (const s of sets)
    for (const b of s.assignment.banks) {
      if (b.mod !== mod) continue;
      const e = files.get(b.file) ?? { meshes: b.meshes, built: b.section, sections: {} };
      e.sections[s.name] = b.resolved;
      files.set(b.file, e);
    }
  return [...files].map(([file, e]) => ({ file, ...e, shifts: new Set(Object.values(e.sections)).size > 1 || Object.values(e.sections).some((v) => v !== e.built) }));
}
