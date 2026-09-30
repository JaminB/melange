import assert from "node:assert/strict";
import { test } from "node:test";
import { assetUrl, isValidPreviewKey, previewExt } from "../../src/sdk/erg";

test("isValidPreviewKey accepts only mesh_/atlas_ keys in the safe charset", () => {
  assert.equal(isValidPreviewKey("mesh_building15"), true);
  assert.equal(isValidPreviewKey("mesh_factory.proj.bazookashell"), true);
  assert.equal(isValidPreviewKey("atlas_camelot"), true);
  assert.equal(isValidPreviewKey(""), false);
  assert.equal(isValidPreviewKey("mesh_"), false);
  assert.equal(isValidPreviewKey("weapon_bazooka"), false, "unknown prefix");
  assert.equal(isValidPreviewKey("mesh_../../etc/passwd"), false, "path traversal");
  assert.equal(isValidPreviewKey("mesh_has space"), false);
  assert.equal(isValidPreviewKey("mesh_HasCaps"), false, "keys are always lower-case");
});

test("previewExt picks the route extension from the key's own prefix", () => {
  assert.equal(previewExt("mesh_building15"), "glb");
  assert.equal(previewExt("atlas_camelot"), "png");
  assert.equal(previewExt("not_a_key"), null);
});

test("assetUrl builds the route, or \"\" for no preview / a bad key", () => {
  assert.equal(assetUrl("mesh_building15"), "/erg/assets/mesh_building15.glb");
  assert.equal(assetUrl("atlas_camelot"), "/erg/assets/atlas_camelot.png");
  assert.equal(assetUrl(""), "", "no preview: the caller draws a labelled box");
  assert.equal(assetUrl("../etc/passwd"), "");
});
