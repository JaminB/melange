import assert from "node:assert/strict";
import { test } from "node:test";
import { optionLabel } from "../../src/launcher/components/SettingControl";
import type { Setting } from "../../src/launcher/api";

const quality: Setting = {
  key: "quality", type: "enum", label: "Quality", default: "bold", options: ["off", "low", "subtle", "bold", "ultra"],
  optionLabels: { bold: { label: "Bold", help: "The full look. Recommended." } },
};

test("optionLabel: uses the declared label when the plugin gives one", () => {
  assert.equal(optionLabel(quality, "bold"), "Bold");
});

test("optionLabel: capitalises an undeclared option instead of showing it lowercase", () => {
  // Sunstone before it declares optionLabels for off/low/subtle/ultra (spec §4.3): Plugins and the wizard must
  // still read "Off"/"Low"/"Subtle"/"Ultra", not the raw lowercase enum value.
  assert.equal(optionLabel(quality, "off"), "Off");
  assert.equal(optionLabel(quality, "low"), "Low");
  assert.equal(optionLabel(quality, "subtle"), "Subtle");
  assert.equal(optionLabel(quality, "ultra"), "Ultra");
});

test("optionLabel: no declaration at all still capitalises", () => {
  const noLabels: Setting = { key: "look", type: "enum", label: "Look", default: "golden", options: ["golden", "dusk"] };
  assert.equal(optionLabel(noLabels, "golden"), "Golden");
  assert.equal(optionLabel(noLabels, "dusk"), "Dusk");
});
