// Opening a project: the pinned base scene (level.load {base}) for the patch, then the project's current scene
// (level.load {project}: the base with the saved patch applied) through the SDK session, which save() needs.
import type { Client } from "../../../sdk/client";
import {
  ErgError, applyPatch, createErgSession, validateScene, type ErgSession, type Patch, type Scene,
} from "../../../sdk/erg";
import { copySurround, emptySurround, surroundBytes, surroundOf } from "../terrain/surround";
import type { Loaded } from "./store";

const TIMEOUT = 30000;

export async function loadBase(client: Client, key: string): Promise<Loaded> {
  const scene = await client.call<Scene>("level.load", { base: key, surround: true }, TIMEOUT);
  const v = validateScene(scene);
  if (!v.ok) throw new ErgError("the server sent an invalid scene", v.errors);
  const blobs = new Map<number, ArrayBuffer>();
  await Promise.all(scene.blobs.map((b) => new Promise<void>((resolve, reject) => {
    const timer = setTimeout(() => { off(); reject(new ErgError(`blob ${b.ref} did not arrive within ${TIMEOUT} ms`)); }, TIMEOUT);
    const off = client.onBinary(b.ref, (data) => {
      off();
      clearTimeout(timer);
      if (data.byteLength !== b.bytes) return reject(new ErgError(`blob ${b.ref}: ${data.byteLength} bytes, expected ${b.bytes}`));
      blobs.set(b.ref, data);
      resolve();
    });
  })));
  return { scene, blobs };
}

export interface Opened { session: ErgSession; base: Loaded; current: Loaded; ms: number; }

export async function openProject(client: Client, project: string, baseKey: string): Promise<Opened> {
  const t0 = performance.now();
  const base = await loadBase(client, baseKey);
  const session = createErgSession(client, { blobTimeoutMs: TIMEOUT, callTimeoutMs: TIMEOUT });
  const current = await session.load(project);
  return { session, base, current, ms: performance.now() - t0 };
}

/** The base with a patch applied, voxels included, as a Loaded the store can take. */
export function withPatch(base: Loaded, patch: Patch): Loaded {
  const voxels = new Map<number, Uint32Array>();
  for (const f of base.scene.frames) {
    const b = f.voxels !== null ? base.blobs.get(f.voxels) : undefined;
    if (b && f.voxels !== null) voxels.set(f.voxels, new Uint32Array(b.slice(0)));
  }
  const baseSurround = surroundOf(base);
  const surround = baseSurround ? copySurround(baseSurround) : emptySurround();
  const scene = applyPatch(base.scene, patch, voxels, surround);
  const blobs = new Map(base.blobs);
  for (const [ref, v] of voxels) blobs.set(ref, v.buffer as ArrayBuffer);
  if (scene.hmp.mode === "paint") {
    const ref = base.scene.hmp.ref ?? Math.max(0, ...base.scene.blobs.map((b) => b.ref + 1));
    scene.hmp.ref = ref;
    blobs.set(ref, surroundBytes(surround));
  }
  return { scene, blobs };
}
