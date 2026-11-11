// render_view {scenery: true}: the open level's scenery details (base pieces and copies) as meshes for render-3d.ts, so a
// decoration pass can be judged offline (no Test): each piece's preview mesh through its detail matrix, in its average
// texture colour. Lights, emitters and sounds are not drawn (no mesh).
import { classifyDetail } from "./sim-fingerprint";
import { placedMeshes, type PlacedMesh } from "./scenery-mesh";
import type { Region } from "./inspect-field";
import type { Session } from "../session";

export async function sceneryMeshesFor(s: Session, clip: Region | null): Promise<{ meshes: PlacedMesh[]; sceneryInfo: { details: number; meshes: number; triangles: number; noMesh: string[] } }> {
  const store = s.store;
  const details = store.scene.details.filter((d) => classifyDetail(d).role === "scenery");
  const within = clip ? (d: (typeof details)[number]) => {
    const p = store.frames.detailWorld(d);
    return p[0] >= clip.x0 && p[0] < clip.x1 && p[2] >= clip.z0 && p[2] < clip.z1;
  } : undefined;
  const { meshes, without } = await placedMeshes(s, store.frames, details, within);
  return { meshes, sceneryInfo: { details: details.length, meshes: meshes.length, triangles: meshes.reduce((n, m) => n + m.tris.length / 9, 0), noMesh: without.slice(0, 20) } };
}
