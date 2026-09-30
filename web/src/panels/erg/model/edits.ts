// Editor commands built on the SDK's: a group of commands as one undo step, moves of several details, duplicates.
import { AddDetail, SetDetail, type Command, type Detail, type DetailFields, type Scene, type Vec3 } from "../../../sdk/erg";

export class Group implements Command {
  constructor(readonly label: string, readonly cmds: Command[], private key = "") {}
  do(s: Scene) { for (const c of this.cmds) c.do(s); }
  undo(s: Scene) { for (let i = this.cmds.length - 1; i >= 0; i--) this.cmds[i].undo(s); }
  /** Two groups with the same key (the same details and fields: one drag) merge pairwise. */
  merge(next: Command): boolean {
    if (!(next instanceof Group) || !this.key || next.key !== this.key || next.cmds.length !== this.cmds.length) return false;
    for (let i = 0; i < this.cmds.length; i++) if (!this.cmds[i].merge?.(next.cmds[i])) return false;
    return true;
  }
}

const fieldKeys = (f: DetailFields) => Object.keys(f).filter((k) => f[k as keyof DetailFields] !== undefined).sort().join(",");

/** Sets fields on several details at once; `changes` pairs a detail id with its new fields. */
export function setMany(changes: [number, DetailFields][], label = "Change details"): Command {
  if (changes.length === 1) return new SetDetail(changes[0][0], changes[0][1], label);
  const key = changes.map(([id, f]) => `${id}:${fieldKeys(f)}`).join(";");
  return new Group(label, changes.map(([id, f]) => new SetDetail(id, f, label)), key);
}

/** Copies of the details in their own frames, offset by `offset` in frame space; the copies' ids are known after do(). */
export class Duplicate implements Command {
  readonly label = "Duplicate";
  private adds: AddDetail[];
  constructor(sources: Detail[], offset: Vec3) {
    this.adds = sources.map((d) => new AddDetail(d.frame, {
      name: d.name, resource: d.resource, pos: [d.pos[0] + offset[0], d.pos[1] + offset[1], d.pos[2] + offset[2]],
      rot: [...d.rot] as Vec3, scale: [...d.scale] as Vec3, voxelPos: [...d.voxelPos] as Vec3,
    }));
  }
  get ids() { return this.adds.map((a) => a.detailId); }
  do(s: Scene) { for (const a of this.adds) a.do(s); }
  undo(s: Scene) { for (let i = this.adds.length - 1; i >= 0; i--) this.adds[i].undo(s); }
}
