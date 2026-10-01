// Runs the mesher in a Web Worker (the page's main thread stays free while a level loads), falling back to the main
// thread when a worker cannot start. Frames are meshed in buckets so a later remesh touches one bucket only.
import { voxelWorld, type Frame, type Scene } from "../../../sdk/erg";
import { emptyMesh, meshFrames, MAX_LEVEL_QUADS, type MeshData, type MeshFrame } from "./mesher";

export const BUCKET = 32;
export const WORKER_URL = "/app/erg-mesher.js";

export interface Bucket { index: number; frames: number[]; }

/** Frames with voxels, in scene order, cut into buckets. */
export function buckets(scene: Scene): Bucket[] {
  const ids = scene.frames.filter((f) => f.voxels !== null && f.size[0] * f.size[1] * f.size[2] > 0).map((f) => f.id);
  const out: Bucket[] = [];
  for (let i = 0; i < ids.length; i += BUCKET) out.push({ index: out.length, frames: ids.slice(i, i + BUCKET) });
  return out;
}

export function meshInput(scene: Scene, frameIds: number[], voxelsOf: (f: Frame) => Uint32Array | undefined): MeshFrame[] {
  const byId = new Map(scene.frames.map((f) => [f.id, f]));
  const out: MeshFrame[] = [];
  for (const id of frameIds) {
    const f = byId.get(id);
    const v = f && voxelsOf(f);
    const world = f && voxelWorld(byId, id);
    if (!f || !v || !world) continue;
    out.push({ id, size: [f.size[0], f.size[1], f.size[2]], world, voxels: v });
  }
  return out;
}

interface Waiting { frames: MeshFrame[]; palette: Uint8Array; maxQuads: number; done: (m: MeshData) => void; }

export class MesherPool {
  private worker?: Worker;
  private job = 0;
  private waiting = new Map<number, Waiting>();

  constructor(url = WORKER_URL) {
    try {
      this.worker = new Worker(url, { type: "module" });
      this.worker.onmessage = (e: MessageEvent<{ job: number; mesh?: MeshData }>) => {
        const w = this.waiting.get(e.data.job);
        if (!w) return;
        this.waiting.delete(e.data.job);
        w.done(e.data.mesh ?? emptyMesh());
      };
      this.worker.onerror = () => this.fallBack();
    } catch {
      this.worker = undefined;
    }
  }

  get threaded() { return !!this.worker; }

  /** Meshes one bucket; the voxel arrays are copied, the caller keeps its own. */
  mesh(bucket: number, frames: MeshFrame[], palette: Uint8Array, maxQuads = MAX_LEVEL_QUADS): Promise<MeshData> {
    if (!this.worker) return Promise.resolve(meshFrames(frames, palette, maxQuads));
    const job = ++this.job;
    return new Promise((done) => {
      this.waiting.set(job, { frames, palette, maxQuads, done });
      const copies = frames.map((f) => ({ ...f, voxels: f.voxels.slice() }));
      this.worker!.postMessage({ job, bucket, frames: copies, palette, maxQuads }, copies.map((f) => f.voxels.buffer as ArrayBuffer));
    });
  }

  private fallBack() {
    this.worker?.terminate();
    this.worker = undefined;
    for (const w of this.waiting.values()) w.done(meshFrames(w.frames, w.palette, w.maxQuads));
    this.waiting.clear();
  }

  dispose() {
    this.worker?.terminate();
    this.worker = undefined;
    this.waiting.clear();
  }
}
