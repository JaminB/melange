// Every edit is a Command (the three.js editor's pattern); the stack lives in the tab.
import {
  KNOT_RESOURCE, deriveRole, type Databank, type Detail, type DetailFields, type HmpMode, type ObjectSpec, type Scene, type SpawnMode,
  type Vec3,
} from "./scene";

export interface Command { label: string; do(s: Scene): void; undo(s: Scene): void; merge?(next: Command): boolean; }

export const MAX_UNDO = 500;

export class CommandStack {
  private done: Command[] = [];
  private undone: Command[] = [];
  private listeners = new Set<() => void>();
  constructor(private scene: Scene, readonly limit = MAX_UNDO) {}

  get canUndo() { return this.done.length > 0; }
  get canRedo() { return this.undone.length > 0; }
  get depth() { return this.done.length; }
  peek(): Command | undefined { return this.done[this.done.length - 1]; }

  /** Runs the command; a command whose merge() takes it into the previous one (a drag) stays one undo step. */
  exec(cmd: Command, mergeable = false) {
    cmd.do(this.scene);
    const last = this.done[this.done.length - 1];
    if (!(mergeable && last?.merge?.(cmd))) {
      this.done.push(cmd);
      if (this.done.length > this.limit) this.done.shift();
    }
    this.undone = [];
    this.emit();
  }
  undo(): boolean {
    const c = this.done.pop();
    if (!c) return false;
    c.undo(this.scene);
    this.undone.push(c);
    this.emit();
    return true;
  }
  redo(): boolean {
    const c = this.undone.pop();
    if (!c) return false;
    c.do(this.scene);
    this.done.push(c);
    this.emit();
    return true;
  }
  clear() {
    this.done = [];
    this.undone = [];
    this.emit();
  }
  onChange(fn: () => void): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }
  private emit() { for (const fn of [...this.listeners]) fn(); }
}

const cloneVec = (v: Vec3): Vec3 => [v[0], v[1], v[2]];
const FIELD_KEYS = ["name", "resource", "pos", "rot", "scale", "voxelPos"] as const;

function detailById(s: Scene, id: number): Detail {
  const d = s.details.find((x) => x.id === id);
  if (!d) throw new Error(`no detail ${id}`);
  return d;
}

function pick(d: Detail, fields: DetailFields): DetailFields {
  const out: DetailFields = {};
  for (const k of FIELD_KEYS) {
    if (fields[k] === undefined) continue;
    const v = d[k];
    (out as Record<string, unknown>)[k] = Array.isArray(v) ? cloneVec(v as Vec3) : v;
  }
  return out;
}

function assign(d: Detail, fields: DetailFields) {
  for (const k of FIELD_KEYS) {
    const v = fields[k];
    if (v === undefined) continue;
    (d as unknown as Record<string, unknown>)[k] = Array.isArray(v) ? cloneVec(v as Vec3) : v;
  }
  d.role = deriveRole(d.name, d.resource);
}

/** Changes fields of one detail. Consecutive changes of the same fields of the same detail merge (a drag). */
export class SetDetail implements Command {
  private before?: DetailFields;
  constructor(readonly id: number, private after: DetailFields, readonly label = "Change detail") {}
  do(s: Scene) {
    const d = detailById(s, this.id);
    if (!this.before) this.before = pick(d, this.after);
    assign(d, this.after);
  }
  undo(s: Scene) { if (this.before) assign(detailById(s, this.id), this.before); }
  merge(next: Command): boolean {
    if (!(next instanceof SetDetail) || next.id !== this.id) return false;
    const keys = (f: DetailFields) => FIELD_KEYS.filter((k) => f[k] !== undefined).join();
    if (keys(next.after) !== keys(this.after)) return false;
    this.after = next.after;
    return true;
  }
}

export class AddDetail implements Command {
  readonly label = "Add detail";
  private id = 0;
  constructor(private frame: number, private fields: DetailFields & { name: string; resource: string; pos: Vec3 }) {}
  get detailId() { return this.id; }
  do(s: Scene) {
    if (!this.id) this.id = s.details.reduce((m, d) => Math.max(m, d.id), 0) + 1;
    const d: Detail = { id: this.id, src: null, frame: this.frame, name: "", resource: "", pos: [0, 0, 0], rot: [0, 0, 0],
      scale: [1, 1, 1], voxelPos: [0, 0, 0], role: "other" };
    assign(d, this.fields);
    s.details.push(d);
  }
  undo(s: Scene) { s.details = s.details.filter((d) => d.id !== this.id); }
}

/** Deletes a detail; an added knot takes its level object with it. */
export class RemoveDetail implements Command {
  readonly label = "Delete detail";
  private removed?: { d: Detail; at: number; object?: { o: ObjectSpec; at: number } };
  constructor(readonly id: number) {}
  do(s: Scene) {
    const at = s.details.findIndex((d) => d.id === this.id);
    if (at < 0) throw new Error(`no detail ${this.id}`);
    const d = s.details[at];
    this.removed = { d, at };
    s.details.splice(at, 1);
    const oi = d.src === null && s.objects ? s.objects.findIndex((o) => o.knot === d.name) : -1;
    if (oi >= 0) {
      this.removed.object = { o: s.objects![oi], at: oi };
      s.objects!.splice(oi, 1);
    }
  }
  undo(s: Scene) {
    if (!this.removed) return;
    s.details.splice(this.removed.at, 0, this.removed.d);
    if (this.removed.object) (s.objects ??= []).splice(this.removed.object.at, 0, this.removed.object.o);
  }
}

/** Places level objects: a knot detail for each (the non-visual marker) and its entry, as one step. */
export class AddObjects implements Command {
  private adds: AddDetail[];
  constructor(frame: number, private specs: ObjectSpec[], positions: Vec3[], readonly label = "Add object") {
    this.adds = specs.map((o, i) => new AddDetail(frame, { name: o.knot, resource: KNOT_RESOURCE, pos: positions[i] }));
  }
  get detailId() { return this.adds[0].detailId; }
  get ids() { return this.adds.map((a) => a.detailId); }
  do(s: Scene) {
    for (const a of this.adds) a.do(s);
    (s.objects ??= []).push(...structuredClone(this.specs));
  }
  undo(s: Scene) {
    const knots = new Set(this.specs.map((o) => o.knot));
    s.objects = (s.objects ?? []).filter((o) => !knots.has(o.knot));
    for (let i = this.adds.length - 1; i >= 0; i--) this.adds[i].undo(s);
  }
}

/** Replaces a level object's settings (its knot and type stay); merges only when exec is asked to. */
export class SetObject implements Command {
  readonly label = "Change object";
  private before?: ObjectSpec;
  constructor(readonly knot: string, private after: ObjectSpec) {}
  private at(s: Scene): number {
    const i = (s.objects ?? []).findIndex((o) => o.knot === this.knot);
    if (i < 0) throw new Error(`no object ${this.knot}`);
    return i;
  }
  do(s: Scene) {
    const i = this.at(s);
    if (!this.before) this.before = structuredClone(s.objects![i]);
    s.objects![i] = structuredClone(this.after);
  }
  undo(s: Scene) { if (this.before) s.objects![this.at(s)] = structuredClone(this.before); }
  merge(next: Command): boolean {
    if (!(next instanceof SetObject) || next.knot !== this.knot) return false;
    this.after = next.after;
    return true;
  }
}

/** Level settings: water (world units, null = default), spawn and surround modes, databank keys, title. */
export class SetLevel implements Command {
  private before?: LevelSettings;
  constructor(private after: LevelSettings, readonly label = "Change level settings") {}
  do(s: Scene) {
    if (!this.before) this.before = readSettings(s, this.after);
    writeSettings(s, this.after);
  }
  undo(s: Scene) { if (this.before) writeSettings(s, this.before); }
  merge(next: Command): boolean {
    if (!(next instanceof SetLevel) || Object.keys(next.after).join() !== Object.keys(this.after).join()) return false;
    this.after = next.after;
    return true;
  }
}

export interface LevelSettings {
  title?: string; water?: number | null; spawns?: SpawnMode; hmp?: HmpMode; databank?: Partial<Databank>;
}

function readSettings(s: Scene, keys: LevelSettings): LevelSettings {
  const out: LevelSettings = {};
  if (keys.title !== undefined) out.title = s.title;
  if (keys.water !== undefined) out.water = s.water.level;
  if (keys.spawns !== undefined) out.spawns = s.spawns.mode;
  if (keys.hmp !== undefined) out.hmp = s.hmp.mode;
  if (keys.databank) {
    out.databank = {};
    for (const k of Object.keys(keys.databank) as (keyof Databank)[]) out.databank[k] = s.databank[k];
  }
  return out;
}

function writeSettings(s: Scene, v: LevelSettings) {
  if (v.title !== undefined) s.title = v.title;
  if (v.water !== undefined) s.water = { level: v.water };
  if (v.spawns !== undefined) s.spawns = { mode: v.spawns };
  if (v.hmp !== undefined) s.hmp = { mode: v.hmp };
  if (v.databank) s.databank = { ...s.databank, ...v.databank };
}

/** Changed voxels of one frame: the words before and after at each index (the terrain brushes build these). */
export class SetVoxels implements Command {
  readonly label = "Sculpt";
  constructor(private voxels: Map<number, Uint32Array>, readonly ref: number, private changes: Map<number, [number, number]>) {}
  do() { this.apply(1); }
  undo() { this.apply(0); }
  private apply(which: 0 | 1) {
    const v = this.voxels.get(this.ref);
    if (!v) throw new Error(`no voxel blob ${this.ref}`);
    for (const [i, pair] of this.changes) v[i] = pair[which];
  }
}
