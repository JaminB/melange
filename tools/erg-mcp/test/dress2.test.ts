// Detail layer round 2 (src/lib/scenery-mesh.ts, render-3d.ts's meshes): a GLB's triangles through its nodes and its flat
// material's colour as sRGB, luminance, and extra meshes drawn two-sided in render_view's rasteriser (the voxel-only
// picture is unchanged without them).
import assert from "node:assert/strict";
import { test } from "node:test";
import { glbMesh, hex, luminance } from "../src/lib/scenery-mesh";
import { LOOKS, renderView, type ViewInput } from "../src/lib/render-3d";

function glb(doc: object, bin: Uint8Array): Uint8Array {
  const js = JSON.stringify(doc), jp = js + " ".repeat((4 - (js.length % 4)) % 4), bp = (4 - (bin.length % 4)) % 4;
  const out = new Uint8Array(12 + 8 + jp.length + 8 + bin.length + bp), v = new DataView(out.buffer);
  v.setUint32(0, 0x46546c67, true); v.setUint32(4, 2, true); v.setUint32(8, out.length, true);
  v.setUint32(12, jp.length, true); v.setUint32(16, 0x4e4f534a, true); out.set(new TextEncoder().encode(jp), 20);
  const b0 = 20 + jp.length;
  v.setUint32(b0, bin.length + bp, true); v.setUint32(b0 + 4, 0x004e4942, true); out.set(bin, b0 + 8);
  return out;
}
function triangleGlb(color: number[]) {
  const pos = new Float32Array([0, 0, 0, 20, 0, 0, 0, 20, 0]), idx = new Uint32Array([0, 1, 2]);
  const bin = new Uint8Array(pos.byteLength + idx.byteLength);
  bin.set(new Uint8Array(pos.buffer), 0); bin.set(new Uint8Array(idx.buffer), pos.byteLength);
  return glb({ scene: 0, scenes: [{ nodes: [0] }], nodes: [{ mesh: 0, translation: [0, 20, 0] }],
    meshes: [{ primitives: [{ attributes: { POSITION: 0 }, indices: 1, material: 0 }] }],
    materials: [{ pbrMetallicRoughness: { baseColorFactor: [...color, 1] } }],
    buffers: [{ byteLength: bin.length }], bufferViews: [{ buffer: 0, byteOffset: 0, byteLength: pos.byteLength }, { buffer: 0, byteOffset: pos.byteLength, byteLength: idx.byteLength }],
    accessors: [{ bufferView: 0, componentType: 5126, count: 3, type: "VEC3", min: [0, 0, 0], max: [20, 20, 0] }, { bufferView: 1, componentType: 5125, count: 3, type: "SCALAR" }] }, bin);
}

test("glbMesh: triangles through the node transform, in the given unit, and the material as sRGB", () => {
  const m = glbMesh(triangleGlb([0.0368, 0.0382, 0.0685]), 20)!;
  assert.ok(m);
  assert.deepEqual([...m.tris], [0, 1, 0, 1, 1, 0, 0, 2, 0]);
  assert.deepEqual(m.min, [0, 1, 0]);
  assert.deepEqual(m.max, [1, 2, 0]);
  assert.equal(hex(m.albedo), "#36374a");
  assert.ok(luminance(m.albedo) < 0.05, "dark basalt");
  assert.equal(luminance([255, 255, 255]), 1);
  assert.equal(glbMesh(new Uint8Array(40)), null);
});

test("render-3d meshes: drawn two-sided where they project, nothing without them", () => {
  const input = (o: Partial<ViewInput> = {}): ViewInput => ({ grids: [], tint: () => [128, 128, 128], look: LOOKS.DAY, water: null, edges: false, ...o });
  const tri = [-5, 0, 0, 5, 0, 0, 0, 8, 0];
  const cam = { pos: [0, 3, 20] as [number, number, number], target: [0, 3, 0] as [number, number, number], fov: 60 };
  const back = { pos: [0, 3, -20] as [number, number, number], target: [0, 3, 0] as [number, number, number], fov: 60 };
  const sky = renderView(input(), cam, 64, 64), front = renderView(input({ meshes: [{ tris: tri, rgb: [200, 40, 40] }] }), cam, 64, 64);
  const rear = renderView(input({ meshes: [{ tris: tri, rgb: [200, 40, 40] }] }), back, 64, 64);
  const px = (v: typeof sky, x: number, y: number) => [...v.rgb.subarray((y * 64 + x) * 3, (y * 64 + x) * 3 + 3)];
  assert.equal(sky.faces, 0);
  assert.equal(front.faces, 1);
  assert.equal(rear.faces, 1, "two-sided");
  const c = px(front, 32, 30);
  assert.ok(c[0] > c[1] * 2, `red where the triangle projects: ${c}`);
  assert.notDeepEqual(px(sky, 32, 30), c);
});
