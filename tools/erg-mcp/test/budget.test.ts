// lib/budget.ts: the mesh pool from a Melange.ini and the headroom arithmetic.
import assert from "node:assert/strict";
import { test } from "node:test";
import { budget, meshPoolFromIni, MELANGE_MESH_POOL, VANILLA_MESH_POOL } from "../src/lib/budget";

test("mesh pool from Melange.ini", () => {
  assert.equal(meshPoolFromIni(null).pool, VANILLA_MESH_POOL);
  assert.equal(meshPoolFromIni("[Fixes]\nEnabled=1\n").pool, MELANGE_MESH_POOL);
  assert.equal(meshPoolFromIni("[Fixes]\nEnabled=1\nMeshInstances=1500   ; comment\n").pool, 1500);
  assert.equal(meshPoolFromIni("[Fixes]\nEnabled=0\nMeshInstances=1500\n").pool, VANILLA_MESH_POOL);
  assert.equal(meshPoolFromIni("[Fixes]\nMeshInstances=0\n").pool, VANILLA_MESH_POOL);
  assert.equal(meshPoolFromIni("[Fixes]\nMeshInstances=99999\n").pool, 4000);
  // another section's key does not count
  assert.equal(meshPoolFromIni("[Other]\nMeshInstances=900\n[Fixes]\nEnabled=1\n").pool, MELANGE_MESH_POOL);
});

test("headroom", () => {
  const b = budget(800, 2000, 250);
  assert.equal(b.headroom, 950); assert.ok(b.ok);
  assert.equal(b.vanillaHeadroom, 550 - 250 - 800);
  assert.ok(!budget(546, 550, 10).ok);
});
