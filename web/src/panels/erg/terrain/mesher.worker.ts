// The mesher's Web Worker: one message per bucket of frames, answered with transferable buffers.
import { meshFrames, type MeshFrame } from "./mesher";

interface Job { job: number; bucket: number; frames: MeshFrame[]; palette: Uint8Array; maxQuads?: number; }

const scope = self as unknown as {
  onmessage: ((e: MessageEvent<Job>) => void) | null;
  postMessage(msg: unknown, transfer: ArrayBuffer[]): void;
};

scope.onmessage = (e) => {
  const { job, bucket, frames, palette, maxQuads } = e.data;
  const t0 = performance.now();
  try {
    const m = meshFrames(frames, palette, maxQuads);
    scope.postMessage({ job, bucket, mesh: m, ms: performance.now() - t0 },
      [m.positions.buffer, m.normals.buffer, m.colors.buffer, m.index.buffer, m.quadFrame.buffer] as ArrayBuffer[]);
  } catch (err) {
    scope.postMessage({ job, bucket, error: String(err) }, []);
  }
};
